// UpdateFetcher over NSURLSession (#199, ADR 0137), for macOS and iOS alike: only API that both
// have (NSURLSession since macOS 10.9 and iOS 7, and GCD). Objective-C++ with ARC.
//
// A data delegate, not a completion handler (#413): the body goes through UpdateBody as it
// arrives, so the task is cancelled once it passes kUpdateMaxResponseBytes (or states a longer
// Content-Length), and a redirect is followed only to an address update_redirect_allowed takes.
#include "update_check.h"

#import <Foundation/Foundation.h>

using maplethief::frontend::update_redirect_allowed;
using maplethief::frontend::UpdateBody;

// One request's state, the session's delegate. Its fields are written on the session's delegate
// queue and read by get() only after `done` was signalled.
@interface MTUpdateFetch : NSObject <NSURLSessionDataDelegate> {
  @public
    UpdateBody body;
}
@property(nonatomic) NSInteger status;
@property(nonatomic) BOOL tooLong;
@property(nonatomic) BOOL redirectRefused;
@property(nonatomic, copy) NSString *failure;
@property(nonatomic, strong) dispatch_semaphore_t done;
@end

@implementation MTUpdateFetch

- (void)URLSession:(NSURLSession *)session
              dataTask:(NSURLSessionDataTask *)dataTask
    didReceiveResponse:(NSURLResponse *)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
        self.status = ((NSHTTPURLResponse *)response).statusCode;
    }
    if (UpdateBody::stated_too_long(response.expectedContentLength)) {
        self.tooLong = YES;
        completionHandler(NSURLSessionResponseCancel);
        return;
    }
    completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession *)session
          dataTask:(NSURLSessionDataTask *)dataTask
    didReceiveData:(NSData *)data {
    __block bool ok = true;
    [data enumerateByteRangesUsingBlock:^(const void *bytes, NSRange range, BOOL *stop) {
      ok = body.append(bytes, range.length);
      *stop = ok ? NO : YES;
    }];
    if (!ok) {
        self.tooLong = YES;
        [dataTask cancel];
    }
}

- (void)URLSession:(NSURLSession *)session
                          task:(NSURLSessionTask *)task
    willPerformHTTPRedirection:(NSHTTPURLResponse *)response
                    newRequest:(NSURLRequest *)request
             completionHandler:(void (^)(NSURLRequest *))completionHandler {
    NSString *to = request.URL.absoluteString;
    if (to != nil && update_redirect_allowed(to.UTF8String)) {
        completionHandler(request);
        return;
    }
    self.redirectRefused = YES;
    completionHandler(nil);
    [task cancel];
}

- (void)URLSession:(NSURLSession *)session
                    task:(NSURLSessionTask *)task
    didCompleteWithError:(NSError *)error {
    if (error != nil) {
        self.failure = error.localizedDescription;
    }
    dispatch_semaphore_signal(self.done);
}

@end

namespace maplethief::frontend {

namespace {

void set_why(std::string *why, const char *text) {
    if (why != nullptr) {
        *why = text != nullptr ? text : "failed";
    }
}

class SessionFetcher final : public UpdateFetcher {
  public:
    std::optional<std::string> get(const std::string &url, std::string *why) override {
        @autoreleasepool {
            NSURL *address = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
            if (address == nil) {
                set_why(why, "not an address");
                return std::nullopt;
            }
            const auto timeout = static_cast<NSTimeInterval>(kUpdateTimeoutSeconds);
            NSMutableURLRequest *request =
                [NSMutableURLRequest requestWithURL:address
                                        cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                    timeoutInterval:timeout];
            [request setValue:[NSString stringWithUTF8String:update_user_agent().c_str()]
                forHTTPHeaderField:@"User-Agent"];
            // Ephemeral: no cookies, no cache, nothing kept on disk.
            NSURLSessionConfiguration *config =
                [NSURLSessionConfiguration ephemeralSessionConfiguration];
            config.timeoutIntervalForResource = timeout;
            MTUpdateFetch *fetch = [[MTUpdateFetch alloc] init];
            fetch.done = dispatch_semaphore_create(0);
            // The session keeps its delegate until it is invalidated, so a late callback after
            // a timeout still finds it.
            NSURLSession *session = [NSURLSession sessionWithConfiguration:config
                                                                  delegate:fetch
                                                             delegateQueue:nil];
            NSURLSessionDataTask *task = [session dataTaskWithRequest:request];
            [task resume];
            const std::int64_t wait_ns =
                static_cast<std::int64_t>(kUpdateTimeoutSeconds + 2) * 1000 * 1000 * 1000;
            const long timed_out =
                dispatch_semaphore_wait(fetch.done, dispatch_time(DISPATCH_TIME_NOW, wait_ns));
            [session finishTasksAndInvalidate];
            if (timed_out != 0) {
                [task cancel];
                set_why(why, "timed out");
                return std::nullopt;
            }
            if (fetch.tooLong) {
                set_why(why, "the response is too long");
                return std::nullopt;
            }
            if (fetch.redirectRefused) {
                set_why(why, "a redirect to a non-HTTPS address was refused");
                return std::nullopt;
            }
            if (fetch.failure != nil) {
                set_why(why, fetch.failure.UTF8String);
                return std::nullopt;
            }
            if (fetch.status < 200 || fetch.status >= 300) {
                set_why(why, ("HTTP status " + std::to_string(fetch.status)).c_str());
                return std::nullopt;
            }
            return fetch->body.take();
        }
    }
};

} // namespace

std::unique_ptr<UpdateFetcher> make_platform_update_fetcher() {
    return std::make_unique<SessionFetcher>();
}

} // namespace maplethief::frontend
