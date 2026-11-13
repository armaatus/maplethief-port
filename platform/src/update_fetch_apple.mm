// UpdateFetcher over NSURLSession (#199, ADR 0137), for macOS and iOS alike: only API that both
// have (NSURLSession since macOS 10.9 and iOS 7, and GCD). Objective-C++ with ARC.
#include "update_check.h"

#import <Foundation/Foundation.h>

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
            NSString *text = [NSString stringWithUTF8String:url.c_str()];
            NSURL *address = text != nil ? [NSURL URLWithString:text] : nil;
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
            NSURLSession *session = [NSURLSession sessionWithConfiguration:config];

            __block NSData *received = nil;
            __block NSInteger status = 0;
            __block NSString *failure = nil;
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            NSURLSessionDataTask *task = [session
                dataTaskWithRequest:request
                  completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
                    if (error != nil) {
                        failure = error.localizedDescription;
                    } else if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
                        status = ((NSHTTPURLResponse *)response).statusCode;
                        received = data;
                    }
                    dispatch_semaphore_signal(done);
                  }];
            [task resume];
            const std::int64_t wait_ns =
                static_cast<std::int64_t>(kUpdateTimeoutSeconds + 2) * 1000 * 1000 * 1000;
            // The check has a thread of its own, which waits for the answer (update_check.h).
            const long timed_out =
                // NOLINTNEXTLINE(clang-analyzer-optin.performance.GCDAntipattern)
                dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, wait_ns));
            [session finishTasksAndInvalidate];
            if (timed_out != 0) {
                [task cancel]; // the handler may still run: its variables are the block's own
                set_why(why, "timed out");
                return std::nullopt;
            }
            if (failure != nil) {
                set_why(why, failure.UTF8String);
                return std::nullopt;
            }
            if (status < 200 || status >= 300 || received == nil) {
                set_why(why, ("HTTP status " + std::to_string(status)).c_str());
                return std::nullopt;
            }
            if (received.length > kUpdateMaxResponseBytes) {
                set_why(why, "the response is too long");
                return std::nullopt;
            }
            return std::string(static_cast<const char *>(received.bytes), received.length);
        }
    }
};

} // namespace

std::unique_ptr<UpdateFetcher> make_platform_update_fetcher() {
    return std::make_unique<SessionFetcher>();
}

} // namespace maplethief::frontend
