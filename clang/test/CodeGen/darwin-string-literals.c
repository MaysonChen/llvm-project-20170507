// RUN: clang-cc -triple i386-apple-darwin9 -emit-llvm %s -o - | FileCheck -check-prefix LSB %s &&

// CHECK-LSB: @.str = private constant [8 x i8] c"string0\00"
// CHECK-LSB: @.str1 = private constant [8 x i8] c"string1\00", section "__TEXT,__cstring,cstring_literals"
// CHECK-LSB: @__utf16_string_ = internal global [35 x i8] c"h\00e\00l\00l\00o\00 \00\92! \00\03& \00\90! \00w\00o\00r\00l\00d\00\00", section "__TEXT,__ustring", align 2

// RUN: clang-cc -triple powerpc-apple-darwin9 -emit-llvm %s -o - | FileCheck -check-prefix MSB %s

// CHECK-MSB: @.str = private constant [8 x i8] c"string0\00"
// CHECK-MSB: @.str1 = private constant [8 x i8] c"string1\00", section "__TEXT,__cstring,cstring_literals"
// CHECK-MSB: @__utf16_string_ = internal global [35 x i8] c"\00h\00e\00l\00l\00o\00 !\92\00 &\03\00 !\90\00 \00w\00o\00r\00l\00d\00", section "__TEXT,__ustring", align 2

const char *g0 = "string0";
const void *g1 = __builtin___CFStringMakeConstantString("string1");
const void *g2 = __builtin___CFStringMakeConstantString("hello \u2192 \u2603 \u2190 world");
