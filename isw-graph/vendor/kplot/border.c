/*	$Id$ */
/*
 * Copyright (c) 2014 Kristaps Dzonsons <kristaps@bsd.lv>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "config.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "kplot.h"
#include "extern.h"

void
kplotctx_border_init(struct kplotctx *ctx)
{
	double		 v;

	kplotctx_line_init(ctx, &ctx->cfg.borderline);
	ISWRenderPathBegin(ctx->rc);

	if (BORDER_LEFT & ctx->cfg.border) {
		v = kplotctx_line_fix(ctx,
			ctx->cfg.borderline.sz, ctx->offs.x);
		ISWRenderPathMoveTo(ctx->rc, v, ctx->offs.y);
		ISWRenderPathLineTo(ctx->rc, v, ctx->offs.y + ctx->dims.y);
	}

	if (BORDER_RIGHT & ctx->cfg.border) {
		v = kplotctx_line_fix(ctx,
			ctx->cfg.borderline.sz,
			ctx->offs.x + ctx->dims.x);
		ISWRenderPathMoveTo(ctx->rc, v, ctx->offs.y);
		ISWRenderPathLineTo(ctx->rc, v, ctx->offs.y + ctx->dims.y);
	}

	if (BORDER_TOP & ctx->cfg.border) {
		v = kplotctx_line_fix(ctx,
			ctx->cfg.borderline.sz, ctx->offs.y);
		ISWRenderPathMoveTo(ctx->rc, ctx->offs.x, v);
		ISWRenderPathLineTo(ctx->rc, ctx->offs.x + ctx->dims.x, v);
	}

	if (BORDER_BOTTOM & ctx->cfg.border) {
		v = kplotctx_line_fix(ctx,
			ctx->cfg.borderline.sz,
			ctx->offs.y + ctx->dims.y);
		ISWRenderPathMoveTo(ctx->rc, ctx->offs.x, v);
		ISWRenderPathLineTo(ctx->rc, ctx->offs.x + ctx->dims.x, v);
	}

	ISWRenderStroke(ctx->rc);
}
