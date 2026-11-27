/* The guest program's Objective-C declarations (#385, ADR 0213): the imported classes and the
 * methods it sends them, declared here from the public ABI (no SDK headers). Only classes and
 * selectors the game itself uses, so the runtime's host classes answer them. */
#ifndef GUEST_OBJC_H
#define GUEST_OBJC_H

#include "guest.h"

typedef struct objc_class *Class;
typedef struct objc_object {
    Class isa;
} *id;
typedef const struct objc_selector *SEL;
#define nil ((id)0)
#define YES ((BOOL)1)
#define NO ((BOOL)0)

typedef struct {
    float x, y;
} CGPoint;
typedef struct {
    float width, height;
} CGSize;
typedef struct {
    CGPoint origin;
    CGSize size;
} CGRect;

__attribute__((objc_root_class))
@interface NSObject {
    Class isa;
}
+ (id)alloc;
+ (Class)class;
- (id)init;
- (void)release;
- (id)retain;
- (id)autorelease;
- (Class)class;
@end

@interface NSString : NSObject
@end

@interface NSNumber : NSObject
+ (id)numberWithBool:(BOOL)value;
@end

@interface NSDictionary : NSObject
+ (id)dictionaryWithObjectsAndKeys:(id)first, ... __attribute__((sentinel));
@end

@interface UIResponder : NSObject
@end

@interface CALayer : NSObject
- (void)setOpaque:(BOOL)opaque;
@end

@interface CAEAGLLayer : CALayer
- (void)setDrawableProperties:(id)properties;
@end

/* UIView's instance is 46 bytes in iOS 5's UIKit, the size the runtime's host class has: a
 * subclass's ivars start after it. */
@interface UIView : UIResponder {
    char _uiview_ivars[42];
}
+ (Class)layerClass;
- (id)initWithFrame:(CGRect)frame;
- (id)layer;
- (void)addSubview:(UIView *)view;
@end

@interface UIWindow : UIView
- (void)makeKeyAndVisible;
@end

@interface EAGLContext : NSObject
+ (BOOL)setCurrentContext:(EAGLContext *)context;
- (id)initWithAPI:(unsigned int)api;
- (BOOL)renderbufferStorage:(unsigned int)target fromDrawable:(id)drawable;
- (BOOL)presentRenderbuffer:(unsigned int)target;
@end
#define kEAGLRenderingAPIOpenGLES1 1u

extern NSString *const kEAGLDrawablePropertyRetainedBacking;
extern NSString *const kEAGLDrawablePropertyColorFormat;
extern NSString *const kEAGLColorFormatRGBA8;

#endif
