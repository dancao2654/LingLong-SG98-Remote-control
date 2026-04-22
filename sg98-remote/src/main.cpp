#if defined(REMOTE_CROWPANEL_128_DIAG)
#include "crowpanel_diag.h"
#else
#include "remote_app.h"
#endif

void setup() {
#if defined(REMOTE_CROWPANEL_128_DIAG)
    crowpanel_diag_setup();
#else
    remote_app_setup();
#endif
}

void loop() {
#if defined(REMOTE_CROWPANEL_128_DIAG)
    crowpanel_diag_loop();
#else
    remote_app_loop();
#endif
}
