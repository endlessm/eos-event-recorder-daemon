/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#ifndef EMTR_UTIL_H
#define EMTR_UTIL_H

#if !(defined(_EMTR_INSIDE_EOSMETRICS_H) || defined(COMPILING_EOS_METRICS))
#error "Please do not include this header file directly."
#endif

#include "emtr-types.h"

#include <gio/gio.h>

G_BEGIN_DECLS

EMTR_ALL_API_VERSIONS
GFile *emtr_get_default_storage_dir (void);

G_END_DECLS

#endif /* EMTR_UTIL_H */
