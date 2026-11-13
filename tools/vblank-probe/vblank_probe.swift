// vblank-probe: measures what macOS tells an app about the display's real vblanks (#366).
//
// Build and run (macOS 14+, a visible window opens for the run's length):
//   swiftc -O tools/vblank-probe/vblank_probe.swift -o build/vblank-probe
//   build/vblank-probe [seconds per phase, default 60]
//
// Phases, one after the other, all printed as one summary block each:
//   info     the screen's refresh facts (NSScreen, CGDisplayMode, CVDisplayLink's nominal and actual period).
//   cvlink   CVDisplayLink: the period between output vblanks, and how late the callback runs.
//   calink   NSView.displayLink (CADisplayLink, macOS 14+): the same for its timestamp.
//   grid     ADR 0075's host grid: a 20 Hz "tick" with 1-9 ms of random work, presented at the first
//            grid boundary after a fixed point, on a grid at the nominal rate with an arbitrary phase.
//            Each present's CAMetalDrawable.presentedTime gives the refreshes it was really shown for.
//   vblank   The same tick, on a grid whose phase and period come from the display link's timestamps.
//
// Synthetic only: it draws a clear colour, never the game.

import AppKit
import CoreVideo
import Metal
import QuartzCore

let tb: mach_timebase_info_data_t = {
    var t = mach_timebase_info_data_t()
    mach_timebase_info(&t)
    return t
}()

func nowNs() -> UInt64 { mach_absolute_time() * UInt64(tb.numer) / UInt64(tb.denom) }
func ticksToNs(_ t: UInt64) -> Double { Double(t) * Double(tb.numer) / Double(tb.denom) }
func nsToTicks(_ ns: UInt64) -> UInt64 { ns * UInt64(tb.denom) / UInt64(tb.numer) }
func sleepUntilNs(_ ns: UInt64) { mach_wait_until(nsToTicks(ns)) }

func pct(_ a: Int, _ b: Int) -> String { b == 0 ? "-" : String(format: "%.2f %%", 100.0 * Double(a) / Double(b)) }

func stats(_ xs: [Double]) -> String {
    guard !xs.isEmpty else { return "n=0" }
    let s = xs.sorted()
    let mean = xs.reduce(0, +) / Double(xs.count)
    let sd = (xs.map { ($0 - mean) * ($0 - mean) }.reduce(0, +) / Double(xs.count)).squareRoot()
    func q(_ p: Double) -> Double { s[min(s.count - 1, Int(p * Double(s.count - 1)))] }
    return String(format: "n=%d mean %.4f sd %.4f min %.4f p1 %.4f p50 %.4f p99 %.4f max %.4f",
                  xs.count, mean, sd, s[0], q(0.01), q(0.5), q(0.99), s[s.count - 1])
}

/// Least-squares line through (index, time): the real period, and the residual of each vblank from it.
func fitPeriod(_ ts: [Double]) -> (period: Double, resid: [Double]) {
    let n = Double(ts.count)
    let xs = (0..<ts.count).map(Double.init)
    let mx = xs.reduce(0, +) / n, my = ts.reduce(0, +) / n
    var sxy = 0.0, sxx = 0.0
    for i in 0..<ts.count { sxy += (xs[i] - mx) * (ts[i] - my); sxx += (xs[i] - mx) * (xs[i] - mx) }
    let k = sxy / sxx, b = my - k * mx
    return (k, (0..<ts.count).map { ts[$0] - (b + k * xs[$0]) })
}

final class Probe: NSObject {
    let seconds: Double
    let window: NSWindow
    let view: NSView
    let layer = CAMetalLayer()
    let device = MTLCreateSystemDefaultDevice()!
    lazy var queue = device.makeCommandQueue()!

    // Display link samples (ns, host time).
    var lock = NSLock()
    var cvSamples: [(now: Double, output: Double, cb: Double)] = []
    var caSamples: [(ts: Double, target: Double, cb: Double)] = []
    var caLink: CADisplayLink?

    init(seconds: Double) {
        self.seconds = seconds
        window = NSWindow(contentRect: NSRect(x: 200, y: 200, width: 320, height: 240),
                          styleMask: [.titled], backing: .buffered, defer: false)
        window.title = "vblank-probe"
        view = NSView(frame: window.contentView!.bounds)
        super.init()
        view.wantsLayer = true
        layer.device = device
        layer.pixelFormat = .bgra8Unorm
        layer.displaySyncEnabled = true
        layer.maximumDrawableCount = 3
        layer.frame = view.bounds
        layer.drawableSize = CGSize(width: 320, height: 240)
        view.layer = layer
        window.contentView = view
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    func info() {
        let screen = window.screen ?? NSScreen.main!
        let id = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as! CGDirectDisplayID
        print("== info")
        print("screen: \(screen.localizedName), \(Int(screen.frame.width))x\(Int(screen.frame.height)) pt")
        print(String(format: "NSScreen.maximumFramesPerSecond %d, minimumRefreshInterval %.6f s, maximumRefreshInterval %.6f s, displayUpdateGranularity %.6f s",
                     screen.maximumFramesPerSecond, screen.minimumRefreshInterval, screen.maximumRefreshInterval,
                     screen.displayUpdateGranularity))
        if let mode = CGDisplayCopyDisplayMode(id) {
            print(String(format: "CGDisplayMode: %dx%d @ %.3f Hz", mode.width, mode.height, mode.refreshRate))
        }
        let opts = [kCGDisplayShowDuplicateLowResolutionModes: kCFBooleanTrue] as CFDictionary
        let modes = (CGDisplayCopyAllDisplayModes(id, opts) as? [CGDisplayMode]) ?? []
        let rates = Set(modes.map { String(format: "%.3f", $0.refreshRate) }).sorted()
        print("refresh rates offered by the display's modes: \(rates.joined(separator: ", "))")
        var link: CVDisplayLink?
        CVDisplayLinkCreateWithCGDisplay(id, &link)
        if let link {
            let nom = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link)
            print(String(format: "CVDisplayLink nominal period %lld/%d = %.9f s (%.6f Hz)", nom.timeValue, nom.timeScale,
                         Double(nom.timeValue) / Double(nom.timeScale), Double(nom.timeScale) / Double(nom.timeValue)))
            CVDisplayLinkStart(link)
            Thread.sleep(forTimeInterval: 0.5)
            let act = CVDisplayLinkGetActualOutputVideoRefreshPeriod(link)
            print(String(format: "CVDisplayLink actual period %.9f s (%.6f Hz)", act, 1 / act))
            CVDisplayLinkStop(link)
        }
    }

    func runCV() {
        var link: CVDisplayLink?
        CVDisplayLinkCreateWithActiveCGDisplays(&link)
        guard let link else { print("== cvlink: no link"); return }
        let me = Unmanaged.passUnretained(self).toOpaque()
        CVDisplayLinkSetOutputCallback(link, { _, now, out, _, _, ctx in
            let cb = ticksToNs(mach_absolute_time())
            let p = Unmanaged<Probe>.fromOpaque(ctx!).takeUnretainedValue()
            p.lock.lock()
            p.cvSamples.append((ticksToNs(now.pointee.hostTime), ticksToNs(out.pointee.hostTime), cb))
            p.lock.unlock()
            return kCVReturnSuccess
        }, me)
        CVDisplayLinkStart(link)
        Thread.sleep(forTimeInterval: seconds)
        CVDisplayLinkStop(link)
        lock.lock(); let s = cvSamples; lock.unlock()
        print("== cvlink (\(s.count) callbacks in \(seconds) s)")
        report(vblanks: s.map { $0.output }, lateness: s.map { ($0.cb - $0.now) / 1e6 }, lead: s.map { ($0.output - $0.cb) / 1e6 })
    }

    @objc func caTick(_ link: CADisplayLink) {
        let cb = ticksToNs(mach_absolute_time())
        lock.lock()
        caSamples.append((link.timestamp * 1e9, link.targetTimestamp * 1e9, cb))
        lock.unlock()
    }

    func runCA() {
        // NSView.displayLink fires on the run loop it is added to; the main run loop is free in this phase.
        DispatchQueue.main.sync {
            let l = view.displayLink(target: self, selector: #selector(caTick(_:)))
            l.add(to: .main, forMode: .common)
            caLink = l
        }
        // A CVDisplayLink alongside, to compare the two sources' phase.
        cvSamples.removeAll()
        var cv: CVDisplayLink?
        CVDisplayLinkCreateWithActiveCGDisplays(&cv)
        let me = Unmanaged.passUnretained(self).toOpaque()
        CVDisplayLinkSetOutputCallback(cv!, { _, now, out, _, _, ctx in
            let p = Unmanaged<Probe>.fromOpaque(ctx!).takeUnretainedValue()
            p.lock.lock()
            p.cvSamples.append((ticksToNs(now.pointee.hostTime), ticksToNs(out.pointee.hostTime), 0))
            p.lock.unlock()
            return kCVReturnSuccess
        }, me)
        CVDisplayLinkStart(cv!)
        Thread.sleep(forTimeInterval: seconds)
        CVDisplayLinkStop(cv!)
        DispatchQueue.main.sync { caLink?.invalidate() }
        lock.lock(); let s = caSamples; let c = cvSamples.map { $0.now }; lock.unlock()
        print("== calink (\(s.count) callbacks in \(seconds) s)")
        report(vblanks: s.map { $0.ts }, lateness: s.map { ($0.cb - $0.ts) / 1e6 }, lead: s.map { ($0.target - $0.cb) / 1e6 })
        // Each CA timestamp against the nearest CVDisplayLink 'now' timestamp, at the start and the end.
        if c.count > 10 {
            let off = s.map { x -> Double in
                var best = Double.infinity
                for y in c where abs(x.ts - y) < abs(best) { best = x.ts - y }
                return best / 1e3
            }
            let head = Array(off.prefix(off.count / 10)), tail = Array(off.suffix(off.count / 10))
            print("CADisplayLink timestamp minus the nearest CVDisplayLink timestamp, first tenth (us): \(stats(head))")
            print("CADisplayLink timestamp minus the nearest CVDisplayLink timestamp, last tenth (us): \(stats(tail))")
        }
    }

    func report(vblanks: [Double], lateness: [Double], lead: [Double]) {
        guard vblanks.count > 10 else { print("too few samples"); return }
        let d = zip(vblanks.dropFirst(), vblanks).map { ($0 - $1) / 1e6 }
        print("interval between vblank timestamps (ms): \(stats(d))")
        // Index each timestamp by its refresh (a skipped callback is a gap of two), then fit.
        let p0 = d.sorted()[d.count / 2]
        var idx = [0.0], acc = 0.0
        for x in d { acc += (x / p0).rounded(); idx.append(acc) }
        let n = Double(vblanks.count)
        let mx = idx.reduce(0, +) / n, my = vblanks.reduce(0, +) / n
        var sxy = 0.0, sxx = 0.0
        for i in 0..<vblanks.count { sxy += (idx[i] - mx) * (vblanks[i] - my); sxx += (idx[i] - mx) * (idx[i] - mx) }
        let k = sxy / sxx
        let resid = (0..<vblanks.count).map { (vblanks[$0] - (my + k * (idx[$0] - mx))) / 1e3 }
        let skipped = Int(acc) + 1 - vblanks.count
        print(String(format: "fitted period %.6f ms = %.6f Hz; %+.1f ppm from 60 Hz; skipped callbacks %d", k / 1e6, 1e9 / k, (k / (1e9 / 60) - 1) * 1e6, skipped))
        print("residual of each timestamp from the fitted grid (us): \(stats(resid))")
        print("callback wake after the link's 'now' timestamp (ms): \(stats(lateness))")
        print("time from callback to the target/output vblank (ms): \(stats(lead))")
    }

    // A 20 Hz tick on a host grid, presented through Metal; presentedTime tells the truth.
    func runTick(name: String, vblankGrid: Bool) {
        // Reference vblanks for the vblank grid: a CVDisplayLink kept running for the phase, its latest output time.
        var refPeriod = 1e9 / 60.0, refPhase = 0.0
        var link: CVDisplayLink?
        if vblankGrid {
            CVDisplayLinkCreateWithActiveCGDisplays(&link)
            cvSamples.removeAll()
            let me = Unmanaged.passUnretained(self).toOpaque()
            CVDisplayLinkSetOutputCallback(link!, { _, now, out, _, _, ctx in
                let p = Unmanaged<Probe>.fromOpaque(ctx!).takeUnretainedValue()
                p.lock.lock()
                p.cvSamples.append((ticksToNs(now.pointee.hostTime), ticksToNs(out.pointee.hostTime), 0))
                if p.cvSamples.count > 600 { p.cvSamples.removeFirst(p.cvSamples.count - 600) }
                p.lock.unlock()
                return kCVReturnSuccess
            }, me)
            CVDisplayLinkStart(link!)
            Thread.sleep(forTimeInterval: 1.0)
        }
        func refresh() {
            guard vblankGrid else { return }
            lock.lock(); let s = cvSamples.map { $0.now }; lock.unlock()
            guard s.count > 30 else { return }
            let f = fitPeriod(s)
            refPeriod = f.period
            refPhase = s.last! - f.resid.last!
        }
        refresh()
        // ADR 0075's grid: period at the nominal 60 Hz, phase = the run's start (unrelated to the display).
        let start = Double(nowNs())
        let gridPeriod = vblankGrid ? refPeriod : 1e9 / 60.0
        let gridPhase = vblankGrid ? refPhase : start
        let n = 3
        let pointMs = 10.0  // the present point: the work's longest (9 ms) plus margin
        var presented: [Double] = []
        var presentCalls: [Double] = []
        var phaseLog: [Double] = []  // vblank grid only: the fitted phase against the first, to see the drift
        let plock = NSLock()
        var pending = 0
        let ticks = Int(seconds * 20)
        var k = Int(((start - gridPhase) / gridPeriod).rounded(.up)) + 1
        var rng = SystemRandomNumberGenerator()
        for t in 0..<ticks {
            if vblankGrid && t % 20 == 0 {
                let oldPhase = refPhase, oldPeriod = refPeriod
                refresh()
                // Re-express k on the refreshed grid so the cadence stays continuous.
                let at = oldPhase + Double(k) * oldPeriod
                k = Int(((at - refPhase) / refPeriod).rounded())
                phaseLog.append(at - (refPhase + Double(k) * refPeriod))
            }
            let p = vblankGrid ? refPeriod : gridPeriod
            let ph = vblankGrid ? refPhase : gridPhase
            let tickStart = ph + Double(k) * p
            sleepUntilNs(UInt64(tickStart))
            // the tick's work: 1-9 ms
            let work = Double.random(in: 1...9, using: &rng) * 1e6
            let w0 = Double(nowNs())
            while Double(nowNs()) - w0 < work {}
            // the present point: tickStart + pointMs (the first grid boundary after it is the next vblank)
            sleepUntilNs(UInt64(tickStart + pointMs * 1e6))
            guard let drawable = layer.nextDrawable() else { continue }
            let cb = queue.makeCommandBuffer()!
            let rp = MTLRenderPassDescriptor()
            rp.colorAttachments[0].texture = drawable.texture
            rp.colorAttachments[0].loadAction = .clear
            rp.colorAttachments[0].clearColor = MTLClearColor(red: Double(t % 2), green: 0.3, blue: 0.5, alpha: 1)
            rp.colorAttachments[0].storeAction = .store
            cb.makeRenderCommandEncoder(descriptor: rp)!.endEncoding()
            drawable.addPresentedHandler { d in
                plock.lock(); presented.append(d.presentedTime * 1e9); pending -= 1; plock.unlock()
            }
            plock.lock(); pending += 1; plock.unlock()
            presentCalls.append(Double(nowNs()))
            cb.present(drawable)
            cb.commit()
            k += n
        }
        Thread.sleep(forTimeInterval: 0.3)
        if let link { CVDisplayLinkStop(link) }
        plock.lock(); let pres = presented.filter { $0 > 0 }; let dropped = presented.count - pres.count; plock.unlock()
        print("== \(name) (\(ticks) ticks, \(pres.count) presented, \(dropped) not presented)")
        if pres.count < ticks / 2 {
            print("most frames never reached the display: the screen is locked, asleep or the window is hidden; no numbers")
            return
        }
        let realP = vblankGrid ? refPeriod : 1e9 / 60.0
        let d = zip(pres.dropFirst(), pres).map { ($0 - $1) / realP }
        var held: [Int: Int] = [:]
        for x in d { held[Int(x.rounded()), default: 0] += 1 }
        print("refreshes each frame was shown for: " + held.keys.sorted().map { "\($0): \(pct(held[$0]!, d.count)) (\(held[$0]!))" }.joined(separator: ", "))
        let lat = zip(pres, presentCalls.prefix(pres.count)).map { ($0 - $1) / 1e6 }
        print("present call to presentedTime (ms): \(stats(lat))")
        if vblankGrid {
            print("phase correction per second (us): \(stats(phaseLog.dropFirst().map { $0 / 1e3 }))")
            print(String(format: "display link period used %.6f ms (%.6f Hz)", refPeriod / 1e6, 1e9 / refPeriod))
        }
        // Where the presents landed against the grid: the real vblanks' phase against the host grid over time.
        if !vblankGrid, pres.count > 20 {
            let off = pres.map { x -> Double in
                let r = (x - gridPhase).truncatingRemainder(dividingBy: gridPeriod)
                return r / 1e6
            }
            let chunk = max(1, off.count / 6)
            let marks = stride(from: 0, to: off.count, by: chunk).map { String(format: "%.3f", off[$0]) }
            print("presentedTime's offset into the nominal grid's refresh, sampled across the run (ms): \(marks.joined(separator: " "))")
        }
    }
}

setvbuf(stdout, nil, _IOLBF, 0)
let seconds = CommandLine.arguments.count > 1 ? Double(CommandLine.arguments[1])! : 60
let app = NSApplication.shared
app.setActivationPolicy(.regular)
let probe = Probe(seconds: seconds)
Thread.detachNewThread {
    Thread.sleep(forTimeInterval: 1.0)
    DispatchQueue.main.sync { probe.info() }
    probe.runCV()
    probe.runCA()
    probe.runTick(name: "grid (ADR 0075: nominal 60 Hz, arbitrary phase)", vblankGrid: false)
    probe.runTick(name: "vblank (CVDisplayLink's period and phase)", vblankGrid: true)
    DispatchQueue.main.async { NSApp.terminate(nil) }
}
app.run()
