// The touch key pad's haptics on iOS (#194, keypad_haptics.h): UIKit's impact generator, light or
// medium. Compiled only for iOS (platform/CMakeLists.txt), with ARC.
//
// The generator is made and prepared when the pad shows (KeypadHapticsTracker's prepare), so the
// first tap does not wait for the Taptic Engine to wake; after each tap it is prepared again, so
// a quick run of presses stays ready; when the pad hides it is let go. A level change makes a new
// one (a generator's style is fixed when it is made). UIKit wants the main thread: the frontend's
// turn hook and the guest's touch delivery run there (SDL's main on iOS).
#import <UIKit/UIKit.h>

#include "keypad_haptics.h"

#if !__has_feature(objc_arc)
#error "keypad_haptics_ios.mm needs ARC (-fobjc-arc)"
#endif

namespace maplethief::frontend {

namespace {

UIImpactFeedbackStyle impact_style(KeypadHaptics level) {
    return level == KeypadHaptics::kMedium ? UIImpactFeedbackStyleMedium
                                           : UIImpactFeedbackStyleLight;
}

class ImpactSink final : public HapticsSink {
  public:
    void prepare(KeypadHaptics level) override {
        if (generator_ == nil || level_ != level) {
            generator_ = [[UIImpactFeedbackGenerator alloc] initWithStyle:impact_style(level)];
            level_ = level;
        }
        [generator_ prepare];
    }

    void impact(KeypadHaptics level) override {
        if (generator_ == nil || level_ != level) {
            prepare(level);
        }
        [generator_ impactOccurred];
        [generator_ prepare]; // ready for the next press
    }

    void release() override { generator_ = nil; }

  private:
    UIImpactFeedbackGenerator *generator_ = nil;
    KeypadHaptics level_ = KeypadHaptics::kOff;
};

} // namespace

std::unique_ptr<HapticsSink> make_platform_haptics_sink() { return std::make_unique<ImpactSink>(); }

} // namespace maplethief::frontend
