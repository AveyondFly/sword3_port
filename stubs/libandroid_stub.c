/* Minimal libandroid.so so Android libSDL2.so can dlopen on glibc. */
static int g_looper, g_window, g_sensor_mgr, g_queue;

void *ALooper_prepare(int opts) {
  (void)opts;
  return &g_looper;
}
void *ALooper_forThread(void) { return &g_looper; }
int ALooper_pollAll(int timeout, int *fd, int *events, void **data) {
  (void)timeout;
  (void)fd;
  (void)events;
  (void)data;
  return -1;
}

void *ANativeWindow_fromSurface(void *env, void *surface) {
  (void)env;
  (void)surface;
  return &g_window;
}
void ANativeWindow_release(void *w) { (void)w; }
int ANativeWindow_getWidth(void *w) {
  (void)w;
  return 640;
}
int ANativeWindow_getHeight(void *w) {
  (void)w;
  return 480;
}
int ANativeWindow_setBuffersGeometry(void *w, int width, int height, int fmt) {
  (void)w;
  (void)width;
  (void)height;
  (void)fmt;
  return 0;
}

void *ASensorManager_getInstance(void) { return &g_sensor_mgr; }
int ASensorManager_getSensorList(void *mgr, void ***list) {
  (void)mgr;
  if (list)
    *list = 0;
  return 0;
}
void *ASensorManager_createEventQueue(void *mgr, void *looper, int ident,
                                      void *cb, void *data) {
  (void)mgr;
  (void)looper;
  (void)ident;
  (void)cb;
  (void)data;
  return &g_queue;
}
int ASensorManager_destroyEventQueue(void *mgr, void *queue) {
  (void)mgr;
  (void)queue;
  return 0;
}
const char *ASensor_getName(void *s) {
  (void)s;
  return "none";
}
int ASensor_getType(void *s) {
  (void)s;
  return 0;
}
int ASensorEventQueue_enableSensor(void *q, void *s) {
  (void)q;
  (void)s;
  return 0;
}
int ASensorEventQueue_disableSensor(void *q, void *s) {
  (void)q;
  (void)s;
  return 0;
}
int ASensorEventQueue_getEvents(void *q, void *ev, int n) {
  (void)q;
  (void)ev;
  (void)n;
  return 0;
}
