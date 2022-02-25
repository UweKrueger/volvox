#include <stdio.h>
#include "str.h"

const char* i1::str() { return v ? "true" : "false"; }

const char* i1::fmt = nullptr;
const char* i1::fmt_w = nullptr;
const char* i1::fmt_wp = nullptr;
