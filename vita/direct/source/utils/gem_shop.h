#ifndef PVZ2_GEM_SHOP_H
#define PVZ2_GEM_SHOP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Results of gem_shop_run. */
enum {
    GEM_SHOP_APPLIED = 1,     /* archive patched on this run */
    GEM_SHOP_ALREADY = 2,     /* this loader's patch was already in place */
    GEM_SHOP_NOT_NEEDED = 3,  /* catalog already sells these plants for gems */
    GEM_SHOP_RESTORED = 4,    /* turned off: original archive restored */
    GEM_SHOP_OFF = 5,         /* turned off and nothing to restore */
    GEM_SHOP_SKIPPED = -1,    /* unexpected archive layout: left unchanged */
    GEM_SHOP_IO_ERROR = -2,
};

/* Make the premium plants in the shop cost gems (enabled) or undo that change
 * (disabled). obb_path is the user's archive; state_path holds the original
 * header bytes and size needed for undo. */
int gem_shop_run(const char *obb_path, const char *state_path, int enabled);

/* Boot entry: applies the change unless DATA_PATH "gem_shop_off.txt" exists. */
void pvz2_gem_shop_boot(void);

#ifdef __cplusplus
}
#endif

#endif
