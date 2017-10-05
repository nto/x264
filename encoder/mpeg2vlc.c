/*****************************************************************************
 * mpeg2vlc.c : mpeg-2 vlc bitstream writing
 *****************************************************************************
 * Copyright (C) 2003-2012 x264 project
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
 *          Phillip Blucas <pblucas@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 * This program is also available under a commercial proprietary license.
 * For more information, contact us at licensing@x264.com.
 *****************************************************************************/

#include "common/common.h"

#ifndef RDO_SKIP_BS
#define RDO_SKIP_BS 0
#endif

#define bs_write_vlc(s,v) bs_write( s, (v).i_size, (v).i_bits )

static void x264_write_dct_vlc_mpeg2( x264_t *h, dctcoef *l, int intra_tab )
{
    bs_t *s = &h->out.bs;
    x264_run_level_t runlevel;
    int i_total = 0;
    const int inter = h->mb.i_type != I_16x16;

    /* minus one because runlevel is zero-indexed and backwards */
    i_total = h->quantf.coeff_level_run[DCT_LUMA_8x8]( l, &runlevel ) - 1;
    if( !inter && runlevel.run[i_total] )
        runlevel.run[i_total]--; // compensate for the DC coefficient set to zero

    for( int i = i_total; i >= 0 && ( runlevel.last > 0 || inter ); i-- )
    {
        if( abs( runlevel.level[i] ) <= 40 && runlevel.run[i] <= dct_vlc_largest_run[abs( runlevel.level[i] )] )
        {
            /* special case in table B.14 for abs(DC) == 1 */
            if( i == i_total && runlevel.run[i] == 0 &&
                ( runlevel.level[i] == 1 || runlevel.level[i] == -1 ) && inter )
                bs_write1( s, 1 );
            else
                bs_write_vlc( s, dct_vlcs[intra_tab][abs( runlevel.level[i] )][runlevel.run[i]] );
            bs_write1( s, runlevel.level[i] < 0 );
        }
        else
        {
            bs_write_vlc( s, dct_vlcs[h->param.b_alt_intra_vlc][0][1] ); // escape
            bs_write( s, 6, runlevel.run[i] );
            bs_write( s, 12, runlevel.level[i] & ((1<<12)-1) ); // convert to 12-bit signed
        }
    }
}

static void x264_write_mv_vlc_mpeg2( x264_t *h, int mvd, int f_code )
{
    bs_t *s = &h->out.bs;
    int r_size = f_code - 1;
    int f = 1 << r_size;
    int low = -(f << 4);
    int high = -low - 1;
    int range =  f << 5;

    if( mvd > high )
        mvd -= range;
    else if( mvd < low  )
        mvd += range;

    int m_residual = abs( mvd ) + f - 1;
    int m_code = m_residual >> r_size;
    if( mvd < 0 )
        m_code = -m_code;
    bs_write_vlc( s, x264_motion_code[m_code + 16] ); // motion_code
    if( r_size && m_code )
        bs_write( s, r_size, m_residual & (f - 1) ); // motion_residual
}

void x264_macroblock_write_vlc_mpeg2( x264_t *h )
{
    bs_t *s = &h->out.bs;
    const int i_mb_type = h->mb.i_type;

#if RDO_SKIP_BS
    s->i_bits_encoded = 0;
#endif

    int cbp420 = h->mb.i_cbp_luma << 2 | h->mb.i_cbp_chroma;
    int cbp = cbp420 << 2 | h->mb.i_cbp_chroma422;
    int coded = !!cbp;
    int quant = h->mb.i_last_qp != h->mb.i_qp && h->mb.i_mb_x;
    int mcoded = !!M32( h->mb.cache.mv[0][X264_SCAN8_0] );
    if( MB_INTERLACED )
        mcoded =  mcoded || (!!M32( h->mb.cache.mv[0][X264_SCAN8_0 + 2*8] ))
               || (h->mb.cache.ref[0][X264_SCAN8_0] != 0)
               || (h->mb.cache.ref[0][X264_SCAN8_0 + 2*8] != 1);
    int mv_type = 0;

    /* must code a zero mv for macroblocks that cannot be (P|B)_SKIP */
    if( !cbp && !mcoded )
        mcoded = 1;

    /* can't use a quant macroblock_type when there are no blocks to code */
    if( quant && !coded )
    {
        quant = 0;
        h->mb.i_qp = h->mb.i_last_qp;
    }

    // macroblock modes
    if( i_mb_type == I_16x16 )
        bs_write_vlc( s, x264_i_mb_type[h->sh.i_type][quant] );
    else if( i_mb_type == P_L0 )
    {
        bs_write_vlc( s, x264_p_mb_type[mcoded][coded][quant] );
        if( !mcoded )
            x264_reset_mv_predictor_mpeg2( h );
    }
    else
    {
        if( i_mb_type == B_L1_L1 )
            mv_type = 1;
        else if( i_mb_type == B_BI_BI )
            mv_type = 2;
        bs_write_vlc( s, x264_b_mb_type[mv_type][coded][quant] );
    }

    if( PARAM_INTERLACED || h->param.b_fake_interlaced )
    {
        /* only frame-based prediction is supported */
        if( (i_mb_type == P_L0 && mcoded) || i_mb_type > P_L0 )
            bs_write( s, 2, 2 - MB_INTERLACED ); // frame|field_motion_type
        if( coded )
            bs_write1( s, MB_INTERLACED ); // dct_type
    }

    if( quant )
        bs_write( s, 5, h->mb.i_qp ); // quantizer_scale_code

#if !RDO_SKIP_BS
    int i_mb_pos_mv = bs_pos( s );
#endif

    // write mvs
    if( (i_mb_type == P_L0 && mcoded) || (i_mb_type != P_L0 && i_mb_type != I_16x16) )
    {
        if( MB_INTERLACED )
        {
            int mvcount = 2;
            mv_type = 0;

            if( i_mb_type == B_BI_BI )
                mvcount = 4;
            else if( i_mb_type == B_L1_L1 )
                mv_type = 1;

            for( int i = 0; i < mvcount; i+=2, mv_type++)
            {
                for( int j = 0; j < 2; j++ )
                {
                    bs_write(s,1,h->mb.cache.ref[mv_type][X264_SCAN8_0 + (2*8*j)]);

                    x264_write_mv_vlc_mpeg2( h, (h->mb.cache.mv[mv_type][X264_SCAN8_0 + (2*8*j)][0]>>1) - (h->mb.mvp[j][mv_type][0]>>1),
                                             h->fenc->mv_fcode[mv_type][0] );
                    x264_write_mv_vlc_mpeg2( h, (h->mb.cache.mv[mv_type][X264_SCAN8_0 + (2*8*j)][1]>>1) - (h->mb.mvp[j][mv_type][1]>>2),
                                             h->fenc->mv_fcode[mv_type][1] );
                    // update predictors
                    h->mb.mvp[j][mv_type][0] = (h->mb.cache.mv[mv_type][X264_SCAN8_0 + (2*8*j)][0]);
                    h->mb.mvp[j][mv_type][1] = (h->mb.cache.mv[mv_type][X264_SCAN8_0 + (2*8*j)][1]>>1)<<2;
                }
            }
        }
        else
        {
            int mvcount = 1;

            mv_type = 0;
            if( i_mb_type == B_BI_BI )
                mvcount = 2;
            else if( i_mb_type == B_L1_L1 )
                mv_type = 1;

            for( int j = 0; j < mvcount; j++, mv_type++ )
            {
                x264_write_mv_vlc_mpeg2( h, ( h->mb.cache.mv[mv_type][x264_scan8[0]][0] - h->mb.mvp[0][mv_type][0] ) >> 1,
                                         h->fenc->mv_fcode[mv_type][0] );
                x264_write_mv_vlc_mpeg2( h, ( h->mb.cache.mv[mv_type][x264_scan8[0]][1] - h->mb.mvp[0][mv_type][1] ) >> 1,
                                         h->fenc->mv_fcode[mv_type][1] );
                // update predictors
                M32( h->mb.mvp[0][mv_type] ) = M32( h->mb.cache.mv[mv_type][X264_SCAN8_0] );
                M32( h->mb.mvp[1][mv_type] ) = M32( h->mb.cache.mv[mv_type][X264_SCAN8_0] );
            }
        }
    }

#if !RDO_SKIP_BS
    int i_mb_pos_tex = bs_pos( s );
    h->stat.frame.i_mv_bits += i_mb_pos_tex - i_mb_pos_mv;
#endif

    // block()
    int blockcount = CHROMA_FORMAT == CHROMA_420 ? 6 : 8;
    if( i_mb_type == I_16x16 )
    {
        for( int i = 0; i < blockcount; i++ )
        {
            h->dct.mpeg2_8x8[i][0] = 0;
            if( i < 4 )
                bs_write_vlc( s, x264_dc_luma_code[h->mb.i_dct_dc_size[i]] );
            else
                bs_write_vlc( s, x264_dc_chroma_code[h->mb.i_dct_dc_size[i]] );

            if( h->mb.i_dct_dc_size[i] )
                bs_write( s, h->mb.i_dct_dc_size[i], h->mb.i_dct_dc_diff[i] ); // DC
            x264_write_dct_vlc_mpeg2( h, h->dct.mpeg2_8x8[i], h->param.b_alt_intra_vlc ); // AC
            bs_write_vlc( s, dct_vlcs[h->param.b_alt_intra_vlc][0][0] ); // end of block
        }
    }
    else if( cbp )
    {
        bs_write_vlc( s, x264_cbp[cbp420] ); // coded_block_pattern_420
        if( CHROMA_FORMAT == CHROMA_422 )
            bs_write( s, 2, cbp & 0x3 );     // coded_block_pattern_1

        for( int i = 0; i < 8; i++ )
        {
            if( cbp & (1<<(7-i)) )
            {
                x264_write_dct_vlc_mpeg2( h, h->dct.mpeg2_8x8[i], 0 ); // DC and AC
                bs_write_vlc( s, dct_vlcs[0][0][0] ); // end of block
            }
        }
    }

#if !RDO_SKIP_BS
    h->stat.frame.i_tex_bits += bs_pos(s) - i_mb_pos_tex;
#endif
}
