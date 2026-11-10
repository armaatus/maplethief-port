// The audio session on iOS (#197, audio_session.h): AVAudioSession's interruptions and the app
// becoming active reach AudioInterruption, and the session is activated again after one.
// Compiled only for iOS (platform/CMakeLists.txt), with ARC.
//
// SDL sets the category and activates the session when its device opens, from
// SDL_HINT_AUDIO_CATEGORY; nothing here sets a category, or SDL would set it back at its next
// open. Reasons and what SDL does itself: ADR 0045's addendum.
#import <AVFAudio/AVFAudio.h>
#import <UIKit/UIKit.h>

#include <SDL3/SDL_log.h>

#include "audio_session.h"

#if !__has_feature(objc_arc)
#error "audio_session_ios.mm needs ARC (-fobjc-arc)"
#endif

using maplethief::frontend::AudioInterruption;

@interface MTAudioSessionObserver : NSObject
@property(nonatomic, assign) AudioInterruption *target;
@end

@implementation MTAudioSessionObserver

- (void)audioSessionInterrupted:(NSNotification *)note {
    @synchronized(self) {
        NSNumber *type = note.userInfo[AVAudioSessionInterruptionTypeKey];
        if (self.target == nullptr || type == nil) {
            return;
        }
        if (type.unsignedIntegerValue == AVAudioSessionInterruptionTypeBegan) {
            self.target->began();
        } else if (type.unsignedIntegerValue == AVAudioSessionInterruptionTypeEnded) {
            self.target->ended("the system says it ended");
        }
    }
}

// An interruption's end is not guaranteed (SDL relies on this too).
- (void)applicationBecameActive:(__unused NSNotification *)note {
    @synchronized(self) {
        if (self.target != nullptr) {
            self.target->ended("the app is active again");
        }
    }
}

@end

namespace maplethief::frontend {

namespace {

class IosAudioSession final : public AudioSessionWatch {
  public:
    explicit IosAudioSession(AudioInterruption *interruption)
        : observer_([MTAudioSessionObserver new]) {
        observer_.target = interruption;
        NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
        [center addObserver:observer_
                   selector:@selector(audioSessionInterrupted:)
                       name:AVAudioSessionInterruptionNotification
                     object:[AVAudioSession sharedInstance]];
        [center addObserver:observer_
                   selector:@selector(applicationBecameActive:)
                       name:UIApplicationDidBecomeActiveNotification
                     object:nil];
    }

    ~IosAudioSession() override {
        [[NSNotificationCenter defaultCenter] removeObserver:observer_];
        @synchronized(observer_) { // a notification under way finishes first
            observer_.target = nullptr;
        }
    }

    bool activate() override {
        @autoreleasepool {
            NSError *error = nil;
            if (![[AVAudioSession sharedInstance] setActive:YES error:&error]) {
                SDL_Log("audio: AVAudioSession refused to activate: %s",
                        error.localizedDescription.UTF8String);
                return false;
            }
            return true;
        }
    }

  private:
    MTAudioSessionObserver *observer_;
};

} // namespace

std::unique_ptr<AudioSessionWatch> make_platform_audio_session(AudioInterruption *interruption) {
    return std::make_unique<IosAudioSession>(interruption);
}

} // namespace maplethief::frontend
