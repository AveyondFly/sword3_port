/* 验证 #1 修复在【繁体 / BIG5】模式下的正确性：
 * 模拟游戏把 BIG5 字节直接喂给 TTF_RenderUTF8_Blended（轩辕剑3天之痕繁体 Windows 版行为）。
 * 通过 shim 的 fopen + SDL_RWFromFile 打开 CT.ttf（shim 据此把 g_ttf_enc 设为 BIG5），
 * 再用 iconv 把已知 UTF-8 串转成 BIG5 字节喂给渲染函数，确认 shim 用 BIG5 转回 UTF-8
 * 并渲染出非空表面。若 enc 显示 BIG5 且 surface 非空，即证明繁体路径正确。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <iconv.h>

static char *utf8_to_big5(const char *u8) {
    iconv_t cd = iconv_open("BIG5", "UTF-8");
    if (cd == (iconv_t)-1) return NULL;
    size_t in = strlen(u8);
    size_t outcap = in * 2 + 8;
    char *out = (char *)malloc(outcap);
    if (!out) { iconv_close(cd); return NULL; }
    char *inbuf = (char *)(uintptr_t)u8; size_t ileft = in;
    char *outbuf = out; size_t oleft = outcap;
    memset(out, 0, outcap);
    if (iconv(cd, &inbuf, &ileft, &outbuf, &oleft) == (size_t)-1) {
        free(out); iconv_close(cd); return NULL;
    }
    iconv_close(cd); *outbuf = '\0'; return out;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <CT.ttf>\n", argv[0]); return 2; }
    const char *fontpath = argv[1];

    if (SDL_Init(SDL_INIT_VIDEO) < 0) { printf("[test] SDL_Init FAIL: %s\n", SDL_GetError()); return 1; }
    if (TTF_Init() < 0)              { printf("[test] TTF_Init FAIL: %s\n", TTF_GetError()); return 1; }

    /* 预热 real_fopen，并经由 shim 的 SDL_RWFromFile 打开字体 -> 设置 g_ttf_enc=BIG5 */
    FILE *warm = fopen(fontpath, "rb"); if (warm) fclose(warm);
    SDL_RWops *rw = SDL_RWFromFile(fontpath, "rb");
    if (!rw) { printf("[test] SDL_RWFromFile('%s') FAIL\n", fontpath); return 1; }
    TTF_Font *font = TTF_OpenFontRW(rw, 1, 30);
    if (!font) { printf("[test] OpenFontRW FAIL\n"); return 1; }

    const char *u8 = "仙劍奇俠傳三";
    char *big5 = utf8_to_big5(u8);
    if (!big5) { printf("[test] utf8->big5 FAIL\n"); return 1; }
    printf("[test] UTF-8 '%s' -> BIG5 bytes:", u8);
    for (int i = 0; big5[i]; i++) printf(" %02x", (unsigned char)big5[i]);
    printf("\n");

    /* 关键：把 BIG5 字节喂给 TTF_RenderUTF8_Blended（模拟游戏），shim 应识别为
     * 非 UTF-8 -> 用 g_ttf_enc(BIG5) 转回 UTF-8 -> CT.ttf 渲染。 */
    SDL_Surface *s = TTF_RenderUTF8_Blended(font, big5, (SDL_Color){255, 255, 255, 255});
    if (!s) { printf("[test] RenderUTF8_Blended(BIG5) -> NULL (FAIL)\n"); return 1; }
    int ok = (s->w > 0 && s->h > 0);
    printf("[test] RenderUTF8_Blended(BIG5) -> w=%d h=%d  non-empty=%s\n",
           s->w, s->h, ok ? "YES (PASS)" : "NO (FAIL)");

    SDL_FreeSurface(s);
    TTF_CloseFont(font);
    TTF_Quit();
    SDL_Quit();
    return ok ? 0 : 1;
}
