#import "MEGAChatError.h"
#import "megachatapi.h"

using namespace megachat;

@interface MEGAChatError()

@property MegaChatError *megaChatError;
@property BOOL cMemoryOwn;

@end

@implementation MEGAChatError

- (instancetype)initWithMegaChatError:(MegaChatError *)megaChatError cMemoryOwn:(BOOL)cMemoryOwn {
    self = [super init];
    
    if (self != nil) {
        _megaChatError = megaChatError;
        _cMemoryOwn = cMemoryOwn;
    }
    
    return self;
}

- (void)dealloc {
    if (self.cMemoryOwn) {
        delete _megaChatError;
    }
}

- (MegaChatError *)getCPtr {
    return self.megaChatError;
}

- (NSString *)description {
    return [NSString stringWithFormat:@"<%@: name=%@, type=%@>",
            [self class], self.name, @(self.type)];
}

- (MEGAChatErrorType)type {
    return (MEGAChatErrorType) (self.megaChatError ? self.megaChatError->getErrorCode() : 1);
}

- (NSString *)name {
    if (!self.megaChatError) return nil;
    const char *ret = self.megaChatError->getErrorString();
    return ret ? [[NSString alloc] initWithUTF8String:ret] : nil;
}

@end
