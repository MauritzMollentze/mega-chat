
#import "MEGAChatPresenceConfig.h"
#import "megachatapi.h"
#import "MEGAChatPresenceConfig+init.h"

using namespace megachat;

@interface MEGAChatPresenceConfig ()

@property MegaChatPresenceConfig *megaChatPresenceConfig;
@property BOOL cMemoryOwn;

@end

@implementation MEGAChatPresenceConfig

- (instancetype)initWithMegaChatPresenceConfig:(megachat::MegaChatPresenceConfig *)megaChatPresenceConfig cMemoryOwn:(BOOL)cMemoryOwn {
    self = [super init];
    
    if (self != nil) {
        _megaChatPresenceConfig = megaChatPresenceConfig;
        _cMemoryOwn = cMemoryOwn;
    }
    
    return self;
}

- (void)dealloc {
    if (self.cMemoryOwn) {
        delete _megaChatPresenceConfig;
    }
}

- (MegaChatPresenceConfig *)getCPtr {
    return self.megaChatPresenceConfig;
}

- (MEGAChatStatus)onlineStatus {
    return (MEGAChatStatus)(self.megaChatPresenceConfig ? self.megaChatPresenceConfig->getOnlineStatus() : 0);
}

- (BOOL)isAutoAwayEnabled {
    return self.megaChatPresenceConfig ? self.megaChatPresenceConfig->isAutoawayEnabled() : NO;
}

- (int64_t)autoAwayTimeout {
    return self.megaChatPresenceConfig ? self.megaChatPresenceConfig->getAutoawayTimeout() : 0;
}

- (BOOL)isPersist {
    return self.megaChatPresenceConfig ? self.megaChatPresenceConfig->isPersist() : NO;
}

- (BOOL)isPending {
    return self.megaChatPresenceConfig ? self.megaChatPresenceConfig->isPending() : NO;
}

- (BOOL)isLastGreenVisible {
    return self.megaChatPresenceConfig ? self.megaChatPresenceConfig->isLastGreenVisible() : NO;
}

@end
