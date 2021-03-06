/*
 * Copyright 2019 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <openssl/core_names.h>
#include "internal/cryptlib.h"
#include "internal/nelem.h"
#include "crypto/evp.h"
#include "crypto/asn1.h"
#include "internal/core.h"
#include "internal/provider.h"
#include "evp_local.h"

struct import_data_st {
    void *provctx;
    void *(*importfn)(const EVP_KEYMGMT *keymgmt, const OSSL_PARAM params[]);

    /* Result */
    void *provdata;
};

static int try_import(const OSSL_PARAM params[], void *arg)
{
    struct import_data_st *data = arg;

    data->provdata = data->importfn(data->provctx, params);
    return data->provdata != NULL;
}

void *evp_keymgmt_export_to_provider(EVP_PKEY *pk, EVP_KEYMGMT *keymgmt,
                                     int want_domainparams)
{
    void *provdata = NULL;
    size_t i, j;

    /*
     * If there is an underlying legacy key and it has changed, invalidate
     * the cache of provider keys.
     */
    if (pk->pkey.ptr != NULL) {
        /*
         * If there is no dirty counter, this key can't be used with
         * providers.
         */
        if (pk->ameth->dirty_cnt == NULL)
            return NULL;

        if (pk->ameth->dirty_cnt(pk) != pk->dirty_cnt_copy)
            evp_keymgmt_clear_pkey_cache(pk);
    }

    /*
     * See if we have exported to this provider already.
     * If we have, return immediately.
     */
    for (i = 0;
         i < OSSL_NELEM(pk->pkeys) && pk->pkeys[i].keymgmt != NULL;
         i++) {
        if (keymgmt == pk->pkeys[i].keymgmt
            && want_domainparams == pk->pkeys[i].domainparams)
            return pk->pkeys[i].provdata;
    }

    if (pk->pkey.ptr != NULL) {
        /* There is a legacy key, try to export that one to the provider */

        /* If the legacy key doesn't have an export function, give up */
        if (pk->ameth->export_to == NULL)
            return NULL;

        /* Otherwise, simply use it. */
        provdata = pk->ameth->export_to(pk, keymgmt, want_domainparams);

        /* Synchronize the dirty count, but only if we exported successfully */
        if (provdata != NULL)
            pk->dirty_cnt_copy = pk->ameth->dirty_cnt(pk);

    } else {
        /*
         * Here, there is no legacy key, so we look at the already cached
         * provider keys, and import from the first that supports it
         * (i.e. use its export function), and export the imported data to
         * the new provider.
         */

        /* Setup for the export callback */
        struct import_data_st import_data;

        import_data.importfn =
            want_domainparams
            ? evp_keymgmt_importdomparams
            : evp_keymgmt_importkey;
        import_data.provdata = NULL;

        /*
         * If the given keymgmt doesn't have an import function, give up
         */
        if (import_data.importfn == NULL)
            return NULL;

        for (j = 0; j < i && pk->pkeys[j].keymgmt != NULL; j++) {
            int (*exportfn)(const EVP_KEYMGMT *keymgmt, void *provdata,
                            OSSL_CALLBACK *cb, void *cbarg) =
                want_domainparams
                ? evp_keymgmt_exportdomparams
                : evp_keymgmt_exportkey;

            if (exportfn != NULL) {
                import_data.provctx =
                    ossl_provider_ctx(EVP_KEYMGMT_provider(keymgmt));

                /*
                 * The export function calls the callback (try_import), which
                 * does the import for us.
                 * Even though we got a success return, we double check that
                 * we actually got something, just in case some implementation
                 * forgets to check the return value.

                 */
                if (exportfn(pk->pkeys[j].keymgmt, pk->pkeys[j].provdata,
                             &try_import, &import_data)
                    && (provdata = import_data.provdata) != NULL)
                    break;
            }
        }
    }

    /*
     * TODO(3.0) Right now, we assume we have ample space.  We will
     * have to think about a cache aging scheme, though, if |i| indexes
     * outside the array.
     */
    if (!ossl_assert(i < OSSL_NELEM(pk->pkeys)))
        return NULL;

    evp_keymgmt_cache_pkey(pk, i, keymgmt, provdata, want_domainparams);

    return provdata;
}

void evp_keymgmt_clear_pkey_cache(EVP_PKEY *pk)
{
    size_t i;

    if (pk != NULL) {
        for (i = 0;
             i < OSSL_NELEM(pk->pkeys) && pk->pkeys[i].keymgmt != NULL;
             i++) {
            EVP_KEYMGMT *keymgmt = pk->pkeys[i].keymgmt;
            void *provdata = pk->pkeys[i].provdata;

            pk->pkeys[i].keymgmt = NULL;
            pk->pkeys[i].provdata = NULL;
            if (pk->pkeys[i].domainparams)
                evp_keymgmt_freedomparams(keymgmt, provdata);
            else
                evp_keymgmt_freekey(keymgmt, provdata);
            EVP_KEYMGMT_free(keymgmt);
        }

        pk->cache.size = 0;
        pk->cache.bits = 0;
        pk->cache.security_bits = 0;
    }
}

void evp_keymgmt_cache_pkey(EVP_PKEY *pk, size_t index, EVP_KEYMGMT *keymgmt,
                            void *provdata, int domainparams)
{
    if (provdata != NULL) {
        EVP_KEYMGMT_up_ref(keymgmt);
        pk->pkeys[index].keymgmt = keymgmt;
        pk->pkeys[index].provdata = provdata;
        pk->pkeys[index].domainparams = domainparams;

        /*
         * Cache information about the domain parameters or key.  Only needed
         * for the "original" provider side key.
         *
         * This services functions like EVP_PKEY_size, EVP_PKEY_bits, etc
         */
        if (index == 0) {
            int ok;
            int bits = 0;
            int security_bits = 0;
            int size = 0;
            OSSL_PARAM params[4];

            params[0] = OSSL_PARAM_construct_int(OSSL_PKEY_PARAM_BITS, &bits);
            params[1] = OSSL_PARAM_construct_int(OSSL_PKEY_PARAM_SECURITY_BITS,
                                                 &security_bits);
            params[2] = OSSL_PARAM_construct_int(OSSL_PKEY_PARAM_MAX_SIZE, &size);
            params[3] = OSSL_PARAM_construct_end();
            ok = domainparams
                ? evp_keymgmt_get_domparam_params(keymgmt, provdata, params)
                : evp_keymgmt_get_key_params(keymgmt, provdata, params);
            if (ok) {
                pk->cache.size = size;
                pk->cache.bits = bits;
                pk->cache.security_bits = security_bits;
            }
        }
    }
}

void *evp_keymgmt_fromdata(EVP_PKEY *target, EVP_KEYMGMT *keymgmt,
                           const OSSL_PARAM params[], int domainparams)
{
    void *provdata = domainparams
        ? evp_keymgmt_importdomparams(keymgmt, params)
        : evp_keymgmt_importkey(keymgmt, params);

    evp_keymgmt_clear_pkey_cache(target);
    evp_keymgmt_cache_pkey(target, 0, keymgmt, provdata, domainparams);

    return provdata;
}

/* internal functions */
/* TODO(3.0) decide if these should be public or internal */
void *evp_keymgmt_importdomparams(const EVP_KEYMGMT *keymgmt,
                                  const OSSL_PARAM params[])
{
    void *provctx = ossl_provider_ctx(EVP_KEYMGMT_provider(keymgmt));

    return keymgmt->importdomparams(provctx, params);
}

void *evp_keymgmt_gendomparams(const EVP_KEYMGMT *keymgmt,
                               const OSSL_PARAM params[])
{
    void *provctx = ossl_provider_ctx(EVP_KEYMGMT_provider(keymgmt));

    return keymgmt->gendomparams(provctx, params);
}

void evp_keymgmt_freedomparams(const EVP_KEYMGMT *keymgmt,
                               void *provdomparams)
{
    keymgmt->freedomparams(provdomparams);
}

int evp_keymgmt_exportdomparams(const EVP_KEYMGMT *keymgmt,
                                void *provdomparams,
                                OSSL_CALLBACK *param_cb, void *cbarg)
{
    return keymgmt->exportdomparams(provdomparams, param_cb, cbarg);
}

const OSSL_PARAM *evp_keymgmt_importdomparam_types(const EVP_KEYMGMT *keymgmt)
{
    return keymgmt->importdomparam_types();
}

/*
 * TODO(v3.0) investigate if we need this function.  'openssl provider' may
 * be a caller...
 */
const OSSL_PARAM *evp_keymgmt_exportdomparam_types(const EVP_KEYMGMT *keymgmt)
{
    return keymgmt->exportdomparam_types();
}

int evp_keymgmt_get_domparam_params(const EVP_KEYMGMT *keymgmt,
                                     void *provdomparams, OSSL_PARAM params[])
{
    if (keymgmt->get_domparam_params == NULL)
        return 1;
    return keymgmt->get_domparam_params(provdomparams, params);
}

const OSSL_PARAM *
evp_keymgmt_gettable_domparam_params(const EVP_KEYMGMT *keymgmt)
{
    if (keymgmt->gettable_domparam_params == NULL)
        return NULL;
    return keymgmt->gettable_domparam_params();
}


void *evp_keymgmt_importkey(const EVP_KEYMGMT *keymgmt,
                            const OSSL_PARAM params[])
{
    void *provctx = ossl_provider_ctx(EVP_KEYMGMT_provider(keymgmt));

    return keymgmt->importkey(provctx, params);
}

void *evp_keymgmt_genkey(const EVP_KEYMGMT *keymgmt, void *domparams,
                         const OSSL_PARAM params[])
{
    void *provctx = ossl_provider_ctx(EVP_KEYMGMT_provider(keymgmt));

    return keymgmt->genkey(provctx, domparams, params);
}

void *evp_keymgmt_loadkey(const EVP_KEYMGMT *keymgmt,
                          void *id, size_t idlen)
{
    void *provctx = ossl_provider_ctx(EVP_KEYMGMT_provider(keymgmt));

    return keymgmt->loadkey(provctx, id, idlen);
}

void evp_keymgmt_freekey(const EVP_KEYMGMT *keymgmt, void *provkey)
{
    keymgmt->freekey(provkey);
}

int evp_keymgmt_exportkey(const EVP_KEYMGMT *keymgmt, void *provkey,
                          OSSL_CALLBACK *param_cb, void *cbarg)
{
    return keymgmt->exportkey(provkey, param_cb, cbarg);
}

const OSSL_PARAM *evp_keymgmt_importkey_types(const EVP_KEYMGMT *keymgmt)
{
    return keymgmt->importkey_types();
}

/*
 * TODO(v3.0) investigate if we need this function.  'openssl provider' may
 * be a caller...
 */
const OSSL_PARAM *evp_keymgmt_exportkey_types(const EVP_KEYMGMT *keymgmt)
{
    return keymgmt->exportkey_types();
}

int evp_keymgmt_get_key_params(const EVP_KEYMGMT *keymgmt,
                               void *provkey, OSSL_PARAM params[])
{
    if (keymgmt->get_key_params == NULL)
        return 1;
    return keymgmt->get_key_params(provkey, params);
}

const OSSL_PARAM *evp_keymgmt_gettable_key_params(const EVP_KEYMGMT *keymgmt)
{
    if (keymgmt->gettable_key_params == NULL)
        return NULL;
    return keymgmt->gettable_key_params();
}

int evp_keymgmt_validate_domparams(const EVP_KEYMGMT *keymgmt, void *provkey)
{
    /* if domainparams are not supported - then pass */
    if (keymgmt->validatedomparams == NULL)
        return 1;
    return keymgmt->validatedomparams(provkey);
}

int evp_keymgmt_validate_public(const EVP_KEYMGMT *keymgmt, void *provkey)
{
    return keymgmt->validatepublic(provkey);
}

int evp_keymgmt_validate_private(const EVP_KEYMGMT *keymgmt, void *provkey)
{
    return keymgmt->validateprivate(provkey);
}

int evp_keymgmt_validate_pairwise(const EVP_KEYMGMT *keymgmt, void *provkey)
{
    return keymgmt->validatepairwise(provkey);
}
