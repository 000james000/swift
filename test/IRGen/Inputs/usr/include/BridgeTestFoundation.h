@class NSString;

@interface NSObject
+ (NSObject*) alloc;
- (NSObject*) init;
+ (NSObject*) new;
+ (void) load;

- (NSString*) description;

@end

@interface NSString : NSObject

- (NSString*)uppercaseString;

@end

@interface Foo

- (NSString*) foo;
- (void) setFoo: (NSString*)s;

@end

NSString *bar(int);
void setBar(NSString *s);

#define CF_ENUM(_type, _name) enum _name : _type _name; enum _name : _type
#define NS_ENUM(_type, _name) CF_ENUM(_type, _name)
