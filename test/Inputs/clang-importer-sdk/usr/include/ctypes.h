typedef enum { red, green, blue } Color;

enum Tribool {
  True, False, Indeterminate
};

enum {
  AnonConst1 = 0x700000000,
  AnonConst2
};

struct Point {
  float x;
  float y;
};

typedef struct {
  struct {
    int a;
    float b;
    struct {
      double c;
    };
  };
} AnonStructs;

typedef struct __NSFastEnumerationState_s {
  unsigned long state;
  void *itemsPtr;
  unsigned long *mutationsPtr;
  unsigned long extra[5];
} NSFastEnumerationState;

typedef void *CFTypeRef;
typedef void const *HWND;
typedef struct __CFString *CFStringRef;

typedef struct {
  struct CGPoint {
    double x;
    double y;
  } origin;
  struct CGSize {
    double width;
    double height;
  } size;
} CGRect;

struct StructWithBitfields {
  unsigned First;
  unsigned Second : 17;
  unsigned Third : 5;
};

//===---
// Tag decls and typedefs.
//===---

struct FooStruct1 {
  int x;
  double y;
};

typedef struct FooStruct2 {
  int x;
  double y;
} FooStructTypedef1;

typedef struct {
  int x;
  double y;
} FooStructTypedef2;

typedef struct FooStruct3 {
  int x;
  double y;
} FooStruct3;

struct FooStruct4 {
  int x;
  double y;
};
typedef struct FooStruct4 FooStruct4;

struct FooStruct5;
typedef struct FooStruct5 FooStruct5;
struct FooStruct5 {
  int x;
  double y;
};

typedef struct FooStruct6 FooStruct6;
struct FooStruct6 {
  int x;
  double y;
};

//===---
// Typedefs.
//===---

typedef CGRect NSRect;

typedef void MyVoid;
MyVoid returnsMyVoid(void);

// Function and struct with same name.
int funcOrStruct(void);
struct funcOrStruct { int i; };

// Names from MacTypes.h that conflict with swift's library types.
// rdar://14175675
typedef unsigned __INT8_TYPE__ UInt8;
typedef unsigned __INT16_TYPE__ UInt16;
typedef unsigned __INT32_TYPE__ UInt32;
typedef unsigned __INT64_TYPE__ UInt64;
typedef float Float32;
typedef double Float64;
typedef long double Float80;

// Other types from MacTypes.h.
typedef __INT8_TYPE__ SInt8;
typedef __INT16_TYPE__ SInt16;
typedef __INT32_TYPE__ SInt32;
typedef __INT64_TYPE__ SInt64;

// Types from stdint.h.
typedef unsigned __INT8_TYPE__ uint8_t;
typedef unsigned __INT16_TYPE__ uint16_t;
typedef unsigned __INT32_TYPE__ uint32_t;
typedef unsigned __INT64_TYPE__ uint64_t;
typedef __INT8_TYPE__ int8_t;
typedef __INT16_TYPE__ int16_t;
typedef __INT32_TYPE__ int32_t;
typedef __INT64_TYPE__ int64_t;
typedef __INTPTR_TYPE__ intptr_t;
typedef unsigned __INTPTR_TYPE__ uintptr_t;

// Types from stddef.h.
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __SIZE_TYPE__ size_t;

// Types from sys/types.h (POSIX).
typedef long ssize_t;

