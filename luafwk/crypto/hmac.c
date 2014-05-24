/*******************************************************************************
 * Copyright (c) 2013 Sierra Wireless and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *   http://www.eclipse.org/legal/epl-v10.html
 * The Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php
 *
 * Contributors:
 *
 *    Fabien Fleutot for Sierra Wireless - initial API and implementation
 *
 ******************************************************************************/

/* Streaming implementation of HMAC-MD5 and HMAC-SHA1, for Lua, which retrieves keys from
 * the keystore, and never writes them in Lua-managed memory.
 *
 * Contrary to the reference implementation in the RFC, it doesn't require
 * the whole signed message to be available simultaneously in RAM.
 *
 * Usage:
 *
 *    hmac = require 'crypto.hmac'
 *    hmac_text1_text2 = M.hmac('md5', key_index) :update(text1) :update(text2) :digest()
 */

#include <strings.h>
#include <string.h>
#include <stddef.h>
#include <malloc.h>
#include "md5.h"
#include "sha1.h"
#include "sha256.h"
#include "lauxlib.h"
#include "keystore.h"

#define MAX_DIGEST_LEN     (256/8)
#define BLOCK_SIZE         (512/8)

struct hmac_ctx_t {
    enum { HASH_MD5, HASH_SHA1, HASH_SHA2 } hash;
    union { MD5_CTX md5; SHA1Context sha1; sha256_context sha2; } u;
    unsigned char key[BLOCK_SIZE];
    int digested;
    int digest_length;
};

static const char figures[]="0123456789abcdef";

static void bin2hex( char *hex, unsigned char *bin, int length) {
    int i;
    for( i=0; i<length; i++) {
        int byte = bin[i];
        hex[2*i]   = figures[byte>>4];
        hex[2*i+1] = figures[byte&0xf];
    }
}

/* Check that value at Lua stack index `idx` is a userdata containing an hmac context. */
static struct hmac_ctx_t *checkhmac ( lua_State *L, int idx) {
    return (struct hmac_ctx_t *) luaL_checkudata( L, idx, "HMAC_CTX");
}

static void h_init( struct hmac_ctx_t *ctx) {
    if( HASH_MD5 == ctx->hash)       MD5Init( & ctx->u.md5);
    else if( HASH_SHA1 == ctx->hash) SHA1Reset( & ctx->u.sha1);
    else if( HASH_SHA2 == ctx->hash) sha256_starts( & ctx->u.sha2);
}

static void h_update( struct hmac_ctx_t *ctx, unsigned char *data, size_t length) {
    if( HASH_MD5 == ctx->hash)       MD5Update( & ctx->u.md5, data, length);
    else if( HASH_SHA1 == ctx->hash) SHA1Input( & ctx->u.sha1, data, length);
    else if( HASH_SHA2 == ctx->hash) sha256_update( & ctx->u.sha2, data, length);
}

static void h_digest( struct hmac_ctx_t *ctx, unsigned char *digest) {
    if( HASH_MD5 == ctx->hash)       MD5Final( digest, & ctx->u.md5);
    else if( HASH_SHA1 == ctx->hash) SHA1Result( & ctx->u.sha1, digest);
    else if( HASH_SHA2 == ctx->hash) sha256_finish( & ctx->u.sha2, digest);
}

/* hmac(hash_name, idx_K) returns a ctx as userdata. */
static int api_hmac( lua_State *L) {
    int i;
    struct hmac_ctx_t *ctx = lua_newuserdata( L, sizeof( * ctx));
    const char *hash_name = luaL_checkstring( L, 1);

    /* Select hash. */
    if( ! strcmp( hash_name, "sha1"))      { ctx->hash = HASH_SHA1; ctx->digest_length = 160/8; }
    else if( ! strcmp( hash_name, "sha2")) { ctx->hash = HASH_SHA2; ctx->digest_length = 256/8; }
    else if( ! strcmp( hash_name, "md5"))  { ctx->hash = HASH_MD5;  ctx->digest_length = 128/8; }
    else { lua_pushnil( L); lua_pushliteral( L, "hash function not supported"); return 2; }

    /* Retrieve the key. */
    bzero( & ctx->key, BLOCK_SIZE);
    if( lua_isnumber( L, 2)) {
      int idx_K = luaL_checkinteger( L, 2)-1;
      i = get_plain_bin_key( idx_K, ctx->key);
      if( i) { lua_pushnil( L); lua_pushinteger( L, i); return 2; }
    } else if( lua_isstring( L, 2)) {
      size_t raw_key_len;
      const char *raw_key = luaL_checklstring( L, 2, & raw_key_len);
      if( raw_key_len > BLOCK_SIZE) {
        lua_pushnil( L); lua_pushliteral( L, "long keys support not implemented"); return 2;
        // TODO: if #key>BLOCK_SIZE then key=hash(key)
      }
      memcpy( ctx->key, raw_key, raw_key_len); // Automatically zero-padded if needed
    } else if(! lua_isnone( L, 2)) {
      luaL_error( L, "2nd argument must be an integer (key index) or a string (key)");
    }

    ctx->digested = 0; // digest not computed yet
    luaL_newmetatable( L, "HMAC_CTX");
    lua_setmetatable( L, -2);
    for( i=0; i<BLOCK_SIZE; i++) ctx->key[i] ^= 0x36; // convert key into k_ipad
    h_init( ctx);
    h_update( ctx, ctx->key, BLOCK_SIZE);
    for( i=0; i<BLOCK_SIZE; i++) ctx->key[i] ^= 0x36 ^ 0x5c; // convert key from k_ipad to k_opad
    return 1;
}

/* hmac_ctx:update(data) returns hmac_ctx, processes data. */
static int api_hmac_update( lua_State *L) {
    struct hmac_ctx_t *ctx = checkhmac ( L, 1);
    size_t datalen;
    unsigned char *data = (unsigned char *) luaL_checklstring( L, 2, & datalen);
    if( ctx->digested) { lua_pushnil( L); lua_pushliteral( L, "digest already computed"); return 2; }
    h_update( ctx, data, datalen);
    lua_pushvalue( L, 1);
    return 1;
}

/* hmac_ctx:digest(b) returns the hmac signature of all data passed to update().
 * Digest is returns as a binary string if `b` is true,
 * as an hexadecimal string if it's false. */
static int api_hmac_digest( lua_State *L) {
    struct hmac_ctx_t *ctx = checkhmac ( L, 1);
    unsigned char digest[MAX_DIGEST_LEN];

    if( ctx->digested) { lua_pushnil( L); lua_pushliteral( L, "digest already computed"); return 2; }
    ctx->digested = 1;

    h_digest( ctx, digest);  // (inner) digest = HASH(k_ipad..msg)
    h_init( ctx);            // reuse hash ctx to compute outer hash
    h_update( ctx, ctx->key, BLOCK_SIZE); // key = k_opad
    h_update( ctx, digest, ctx->digest_length);
    h_digest(ctx, digest); // (outer) digest = HASH(k_opad..inner digest)

    if( lua_toboolean( L, 2)) {
        lua_pushlstring( L, (const char *) digest, ctx->digest_length);
    } else {
        char hex[2*MAX_DIGEST_LEN];
        bin2hex( hex, digest, ctx->digest_length);
        lua_pushlstring( L, (const char *) hex, 2*ctx->digest_length);
    }

    return 1;
}

/* Helper for ltn12 filter. */
static int hmac_filter_closure( lua_State *L) {
    struct hmac_ctx_t *ctx = checkhmac ( L, lua_upvalueindex( 1));
    if( ctx->digested) { lua_pushnil( L); lua_pushliteral( L, "digest already computed"); return 2; }
    if( ! lua_isnil( L, 1)) {
        size_t size;
        unsigned char *data = (unsigned char *) luaL_checklstring( L, 1, & size);
        h_update( ctx, data, size);
    }
    /* return the original argument, unmodified. */
    lua_settop( L, 1);
    return 1;
}

/* Returns an ltn2 filter, which lets data go through it unmodified but updates the hmac context. */
static int api_hmac_filter( lua_State *L) {
    checkhmac ( L, 1);
    lua_settop( L, 1);
    lua_pushcclosure( L, hmac_filter_closure, 1);
    return 1;
}

int luaopen_crypto_hmac( lua_State *L) {
#define REG(c_name, lua_name) lua_pushcfunction( L, c_name); lua_setfield( L, -2, lua_name)

    /* Build HMAC metatable. */
    luaL_newmetatable( L, "HMAC_CTX"); // hmac_mt
    lua_newtable( L);                  // hmac_mt, hmac_index
    lua_pushvalue( L, -1);             // hmac_mt, hmac_index, hmac_index
    lua_setfield( L, -3, "__index");   // hmac_mt[__index=hmac_index], hmac_index
    REG( api_hmac_update, "update");
    REG( api_hmac_digest, "digest");
    REG( api_hmac_filter, "filter");
    lua_pop( L, 2);// -

    /* Return constructor. */
    lua_pushcfunction( L, api_hmac);
    return 1;
#undef REG
}
