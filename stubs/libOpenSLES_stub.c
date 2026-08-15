/* Minimal libOpenSLES.so so Android libSDL2.so can dlopen on glibc.
 * Real audio still goes through the loader's opensles_shim / device SDL2. */
typedef int SLresult;
static int id_engine, id_play, id_volume, id_bq;
const void *SL_IID_ENGINE = &id_engine;
const void *SL_IID_PLAY = &id_play;
const void *SL_IID_VOLUME = &id_volume;
const void *SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &id_bq;

SLresult slCreateEngine(void **engine, int nopt, const void *opt, int nint,
                        const void *iids, const void *req) {
  (void)nopt;
  (void)opt;
  (void)nint;
  (void)iids;
  (void)req;
  if (engine)
    *engine = (void *)1;
  return 0;
}
