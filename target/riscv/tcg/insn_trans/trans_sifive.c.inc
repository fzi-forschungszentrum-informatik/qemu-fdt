static bool trans_cflush_d_l1(DisasContext *ctx, arg_cflush_d_l1 *a) {
    /* dummy l1 cache flush */
    return true;
}

static bool trans_cdiscard_d_l1(DisasContext *ctx, arg_cdiscard_d_l1 *a) {
    /* dummy l1 cache discard */
    return true;
}
