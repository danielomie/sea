/*
 * libs/jwt/jwt_native.c — JWT crypto primitives for Sea.
 *
 * Build:  cd libs/jwt && make
 *
 * Exported functions (consumed by jwt.sea):
 *   base64url_encode(data)              → string
 *   base64url_decode(data)              → string | nil
 *   hs_sign(data, secret, alg)          → b64url_signature | nil
 *   hs_verify(data, sig_b64url, secret, alg) → bool
 *   rs256_sign(data, pem_privkey)       → b64url_signature | nil
 *   rs256_verify(data, sig_b64url, pem_pubkey) → bool
 */

#include "sea.h"
#include "sea_rope.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- Base64URL ------------------------------------------------------------ */

static const char B64URL[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t b64url_enc(const unsigned char *src, size_t slen, char *dst) {
    size_t j = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < slen; i++) {
        acc = (acc << 8) | src[i];
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            dst[j++] = B64URL[(acc >> bits) & 0x3F];
        }
    }
    if (bits > 0)
        dst[j++] = B64URL[(acc << (6 - bits)) & 0x3F];
    return j;
}

static int b64url_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

/* Returns decoded length, or -1 on invalid character. */
static ssize_t b64url_dec(const char *src, size_t slen,
                           unsigned char *dst, size_t dmax) {
    size_t dlen = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < slen; i++) {
        if (src[i] == '=') break;
        int v = b64url_char(src[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (dlen >= dmax) return -1;
            dst[dlen++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    return (ssize_t)dlen;
}

/* ---- Sea helpers ---------------------------------------------------------- */

static SeaString *get_str(SeaValue v) {
    return sea_value_get_flat_string(v);
}

/* ---- fn: base64url_encode(data) → string --------------------------------- */

static SeaValue fn_base64url_encode(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaString *s = get_str(args[0]);
    if (!s) return SEA_NIL_VAL;

    size_t enc_cap = ((s->len + 2) / 3) * 4 + 1;
    char *buf = malloc(enc_cap);
    if (!buf) { sea_obj_release(&s->obj); return SEA_NIL_VAL; }

    size_t enc_len = b64url_enc((const unsigned char*)s->data, s->len, buf);
    SeaValue r = sea_obj_val(&sea_string_new(buf, enc_len)->obj);
    free(buf);
    sea_obj_release(&s->obj);
    return r;
}

/* ---- fn: base64url_decode(data) → string | nil --------------------------- */

static SeaValue fn_base64url_decode(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaString *s = get_str(args[0]);
    if (!s) return SEA_NIL_VAL;

    size_t dec_cap = (s->len * 3) / 4 + 4;
    unsigned char *buf = malloc(dec_cap);
    if (!buf) { sea_obj_release(&s->obj); return SEA_NIL_VAL; }

    ssize_t dec_len = b64url_dec(s->data, s->len, buf, dec_cap);
    sea_obj_release(&s->obj);
    if (dec_len < 0) { free(buf); return SEA_NIL_VAL; }

    SeaValue r = sea_obj_val(&sea_string_new((char*)buf, (size_t)dec_len)->obj);
    free(buf);
    return r;
}

/* ---- HMAC helpers --------------------------------------------------------- */

static const EVP_MD *get_md(const char *alg) {
    if (strcmp(alg, "HS256") == 0) return EVP_sha256();
    if (strcmp(alg, "HS384") == 0) return EVP_sha384();
    if (strcmp(alg, "HS512") == 0) return EVP_sha512();
    return NULL;
}

/* Constant-time byte comparison (avoid timing leaks in HMAC verify). */
static int ct_memeq(const unsigned char *a, const unsigned char *b, size_t n) {
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

/* ---- fn: hs_sign(data, secret, alg) → b64url | nil ---------------------- */

static SeaValue fn_hs_sign(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 3) return SEA_NIL_VAL;
    SeaString *data   = get_str(args[0]);
    SeaString *secret = get_str(args[1]);
    SeaString *alg_s  = get_str(args[2]);
    if (!data || !secret || !alg_s) {
        if (data)   sea_obj_release(&data->obj);
        if (secret) sea_obj_release(&secret->obj);
        if (alg_s)  sea_obj_release(&alg_s->obj);
        return SEA_NIL_VAL;
    }

    const EVP_MD *md = get_md(alg_s->data);
    sea_obj_release(&alg_s->obj);
    if (!md) { sea_obj_release(&data->obj); sea_obj_release(&secret->obj); return SEA_NIL_VAL; }

    unsigned char sig[64];
    unsigned int siglen = 0;
    HMAC(md,
         secret->data, (int)secret->len,
         (const unsigned char*)data->data, data->len,
         sig, &siglen);
    sea_obj_release(&data->obj);
    sea_obj_release(&secret->obj);

    size_t enc_cap = ((siglen + 2) / 3) * 4 + 1;
    char *buf = malloc(enc_cap);
    if (!buf) return SEA_NIL_VAL;
    size_t enc_len = b64url_enc(sig, siglen, buf);
    SeaValue r = sea_obj_val(&sea_string_new(buf, enc_len)->obj);
    free(buf);
    return r;
}

/* ---- fn: hs_verify(data, sig_b64url, secret, alg) → bool ---------------- */

static SeaValue fn_hs_verify(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 4) return sea_bool_val(false);
    SeaString *data   = get_str(args[0]);
    SeaString *sig_s  = get_str(args[1]);
    SeaString *secret = get_str(args[2]);
    SeaString *alg_s  = get_str(args[3]);
    if (!data || !sig_s || !secret || !alg_s) {
        if (data)   sea_obj_release(&data->obj);
        if (sig_s)  sea_obj_release(&sig_s->obj);
        if (secret) sea_obj_release(&secret->obj);
        if (alg_s)  sea_obj_release(&alg_s->obj);
        return sea_bool_val(false);
    }

    const EVP_MD *md = get_md(alg_s->data);
    sea_obj_release(&alg_s->obj);
    if (!md) {
        sea_obj_release(&data->obj); sea_obj_release(&sig_s->obj);
        sea_obj_release(&secret->obj);
        return sea_bool_val(false);
    }

    /* Decode the provided signature */
    unsigned char provided[64];
    ssize_t plen = b64url_dec(sig_s->data, sig_s->len, provided, sizeof(provided));
    sea_obj_release(&sig_s->obj);
    if (plen < 0) {
        sea_obj_release(&data->obj); sea_obj_release(&secret->obj);
        return sea_bool_val(false);
    }

    /* Recompute HMAC */
    unsigned char expected[64];
    unsigned int elen = 0;
    HMAC(md,
         secret->data, (int)secret->len,
         (const unsigned char*)data->data, data->len,
         expected, &elen);
    sea_obj_release(&data->obj);
    sea_obj_release(&secret->obj);

    bool ok = (size_t)plen == elen && ct_memeq(provided, expected, elen);
    return sea_bool_val(ok);
}

/* ---- fn: rs256_sign(data, pem_privkey) → b64url | nil ------------------- */

static SeaValue fn_rs256_sign(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2) return SEA_NIL_VAL;
    SeaString *data = get_str(args[0]);
    SeaString *pem  = get_str(args[1]);
    if (!data || !pem) {
        if (data) sea_obj_release(&data->obj);
        if (pem)  sea_obj_release(&pem->obj);
        return SEA_NIL_VAL;
    }

    BIO *bio = BIO_new_mem_buf(pem->data, (int)pem->len);
    sea_obj_release(&pem->obj);
    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (!pkey) { sea_obj_release(&data->obj); return SEA_NIL_VAL; }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    SeaValue r = SEA_NIL_VAL;
    if (EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, pkey) == 1 &&
        EVP_DigestSignUpdate(ctx, data->data, data->len) == 1) {
        size_t siglen = 0;
        EVP_DigestSignFinal(ctx, NULL, &siglen);
        unsigned char *sig = malloc(siglen);
        if (sig && EVP_DigestSignFinal(ctx, sig, &siglen) == 1) {
            size_t enc_cap = ((siglen + 2) / 3) * 4 + 1;
            char *buf = malloc(enc_cap);
            if (buf) {
                size_t enc_len = b64url_enc(sig, siglen, buf);
                r = sea_obj_val(&sea_string_new(buf, enc_len)->obj);
                free(buf);
            }
        }
        free(sig);
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    sea_obj_release(&data->obj);
    return r;
}

/* ---- fn: rs256_verify(data, sig_b64url, pem_pubkey) → bool -------------- */

static SeaValue fn_rs256_verify(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 3) return sea_bool_val(false);
    SeaString *data   = get_str(args[0]);
    SeaString *sig_s  = get_str(args[1]);
    SeaString *pem    = get_str(args[2]);
    if (!data || !sig_s || !pem) {
        if (data)  sea_obj_release(&data->obj);
        if (sig_s) sea_obj_release(&sig_s->obj);
        if (pem)   sea_obj_release(&pem->obj);
        return sea_bool_val(false);
    }

    /* Decode signature */
    size_t sig_cap = (sig_s->len * 3) / 4 + 4;
    unsigned char *sig = malloc(sig_cap);
    ssize_t siglen = sig ? b64url_dec(sig_s->data, sig_s->len, sig, sig_cap) : -1;
    sea_obj_release(&sig_s->obj);
    if (siglen < 0) {
        free(sig);
        sea_obj_release(&data->obj); sea_obj_release(&pem->obj);
        return sea_bool_val(false);
    }

    BIO *bio = BIO_new_mem_buf(pem->data, (int)pem->len);
    sea_obj_release(&pem->obj);
    EVP_PKEY *pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
    BIO_free(bio);
    bool ok = false;
    if (pkey) {
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        if (EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, pkey) == 1 &&
            EVP_DigestVerifyUpdate(ctx, data->data, data->len) == 1 &&
            EVP_DigestVerifyFinal(ctx, sig, (size_t)siglen) == 1)
            ok = true;
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
    }
    free(sig);
    sea_obj_release(&data->obj);
    return sea_bool_val(ok);
}

/* ---- Module entry point --------------------------------------------------- */

SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
    (void)vm;
    SeaTable *mod = sea_mod_new();
    sea_mod_set(mod, "base64url_encode", fn_base64url_encode);
    sea_mod_set(mod, "base64url_decode", fn_base64url_decode);
    sea_mod_set(mod, "hs_sign",          fn_hs_sign);
    sea_mod_set(mod, "hs_verify",        fn_hs_verify);
    sea_mod_set(mod, "rs256_sign",       fn_rs256_sign);
    sea_mod_set(mod, "rs256_verify",     fn_rs256_verify);
    return sea_obj_val(&mod->obj);
}
