#pragma once
// Clean checkouts use placeholders and runtime provisioning. Private legacy
// credentials remain optional and must never be added to version control.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.h.example"
#endif
