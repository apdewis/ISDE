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
#include <stdio.h>
#include <stdlib.h>

#include "kplot.h"
#include "extern.h"

void
kplotctx_tic_init(struct kplotctx *ctx)
{
	double		 offs, v;
	size_t		 i;

	kplotctx_ticln_init(ctx, &ctx->cfg.ticline);
	ISWRenderPathBegin(ctx->rc);

	for (i = 0; i < ctx->cfg.xtics; i++) {
		offs = 1 == ctx->cfg.xtics ? 0.5 :
			i / (double)(ctx->cfg.xtics - 1);
		v = kplotctx_line_fix(ctx,
			ctx->cfg.ticline.sz,
			ctx->offs.x + offs * ctx->dims.x);
		if (TIC_BOTTOM_IN & ctx->cfg.tic) {
			ISWRenderPathMoveTo(ctx->rc, v,
				ctx->offs.y + ctx->dims.y);
			ISWRenderPathLineTo(ctx->rc, v,
				ctx->offs.y + ctx->dims.y - ctx->cfg.ticline.len);
		}
		if (TIC_BOTTOM_OUT & ctx->cfg.tic) {
			ISWRenderPathMoveTo(ctx->rc, v,
				ctx->offs.y + ctx->dims.y);
			ISWRenderPathLineTo(ctx->rc, v,
				ctx->offs.y + ctx->dims.y + ctx->cfg.ticline.len);
		}
		if (TIC_TOP_IN & ctx->cfg.tic) {
			ISWRenderPathMoveTo(ctx->rc, v, ctx->offs.y);
			ISWRenderPathLineTo(ctx->rc, v,
				ctx->offs.y + ctx->cfg.ticline.len);
		}
		if (TIC_TOP_OUT & ctx->cfg.tic) {
			ISWRenderPathMoveTo(ctx->rc, v, ctx->offs.y);
			ISWRenderPathLineTo(ctx->rc, v,
				ctx->offs.y - ctx->cfg.ticline.len);
		}
	}

	for (i = 0; i < ctx->cfg.ytics; i++) {
		offs = 1 == ctx->cfg.ytics ? 0.5 :
			i / (double)(ctx->cfg.ytics - 1);
		v = kplotctx_line_fix(ctx,
			ctx->cfg.ticline.sz,
			ctx->offs.y + offs * ctx->dims.y);
		if (TIC_LEFT_IN & ctx->cfg.tic) {
			ISWRenderPathMoveTo(ctx->rc, ctx->offs.x, v);
			ISWRenderPathLineTo(ctx->rc,
				ctx->offs.x + ctx->cfg.ticline.len, v);
		}
		if (TIC_LEFT_OUT & ctx->cfg.tic) {
			ISWRenderPathMoveTo(ctx->rc, ctx->offs.x, v);
			ISWRenderPathLineTo(ctx->rc,
				ctx->offs.x - ctx->cfg.ticline.len, v);
		}
		if (TIC_RIGHT_IN & ctx->cfg.tic) {
			ISWRenderPathMoveTo(ctx->rc,
				ctx->offs.x + ctx->dims.x, v);
			ISWRenderPathLineTo(ctx->rc,
				ctx->offs.x + ctx->dims.x - ctx->cfg.ticline.len, v);
		}
		if (TIC_RIGHT_OUT & ctx->cfg.tic) {
			ISWRenderPathMoveTo(ctx->rc,
				ctx->offs.x + ctx->dims.x, v);
			ISWRenderPathLineTo(ctx->rc,
				ctx->offs.x + ctx->dims.x + ctx->cfg.ticline.len, v);
		}
	}

	ISWRenderStroke(ctx->rc);
}
