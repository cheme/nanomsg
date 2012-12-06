/*
    Copyright (c) 2012 250bpm s.r.o.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "respondent.h"
#include "xrespondent.h"

#include "../../sp.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/alloc.h"

#include <stdint.h>

#define SP_RESPONDENT_INPROGRESS 1

struct sp_respondent {
    struct sp_xrespondent xrespondent;
    uint32_t surveyid;
    uint32_t flags;
};

/*  Implementation of sp_sockbase's virtual functions. */
static void sp_respondent_term (struct sp_sockbase *self);
static int sp_respondent_send (struct sp_sockbase *self, const void *buf,
    size_t len);
static int sp_respondent_recv (struct sp_sockbase *self, void *buf,
    size_t *len);
static const struct sp_sockbase_vfptr sp_respondent_sockbase_vfptr = {
    sp_respondent_term,
    sp_xrespondent_add,
    sp_xrespondent_rm,
    sp_xrespondent_in,
    sp_xrespondent_out,
    sp_respondent_send,
    sp_respondent_recv,
    sp_xrespondent_setopt,
    sp_xrespondent_getopt
};

static void sp_respondent_init (struct sp_respondent *self,
    const struct sp_sockbase_vfptr *vfptr, int fd)
{
    sp_xrespondent_init (&self->xrespondent, vfptr, fd);
    self->flags = 0;
}

void sp_respondent_term (struct sp_sockbase *self)
{
    struct sp_respondent *respondent;

    respondent = sp_cont (self, struct sp_respondent, xrespondent.sockbase);

    sp_xrespondent_term (self);
}

static int sp_respondent_send (struct sp_sockbase *self, const void *buf,
    size_t len)
{
    struct sp_respondent *respondent;

    respondent = sp_cont (self, struct sp_respondent, xrespondent.sockbase);

    sp_assert (0);
}

static int sp_respondent_recv (struct sp_sockbase *self, void *buf, size_t *len)
{
    struct sp_respondent *respondent;

    respondent = sp_cont (self, struct sp_respondent, xrespondent.sockbase);

    sp_assert (0);
}

static struct sp_sockbase *sp_respondent_create (int fd)
{
    struct sp_respondent *self;

    self = sp_alloc (sizeof (struct sp_respondent));
    alloc_assert (self);
    sp_respondent_init (self, &sp_respondent_sockbase_vfptr, fd);
    return &self->xrespondent.sockbase;
}

static struct sp_socktype sp_respondent_socktype_struct = {
    AF_SP,
    SP_RESPONDENT,
    sp_respondent_create
};

struct sp_socktype *sp_respondent_socktype = &sp_respondent_socktype_struct;
