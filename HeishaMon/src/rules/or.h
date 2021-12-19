/*
  Copyright (C) CurlyMo

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#ifndef _RULES_OR_H_
#define _RULES_OR_H_

#include "rules.h"

int event_operator_or_callback(struct rules_t *obj, int a, int b, int *ret);

#endif