/******************************************************************************
 * $Id$
 *
 * Project:  Image Warper
 * Purpose:  Implements a rational polynomail (RPC) based transformer. 
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2003, Frank Warmerdam <warmerdam@pobox.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "gdal_priv.h"
#include "gdal_alg.h"
#include "ogr_spatialref.h"

CPL_CVSID("$Id$");

/************************************************************************/
/*                          RPCComputeTerms()                           */
/************************************************************************/

static void RPCComputeTerms( double dfLong, double dfLat, double dfHeight,
                             double *padfTerms )

{
    padfTerms[0] = 1.0;
    padfTerms[1] = dfLong;
    padfTerms[2] = dfLat;
    padfTerms[3] = dfHeight;
    padfTerms[4] = dfLong * dfLat;
    padfTerms[5] = dfLong * dfHeight;
    padfTerms[6] = dfLat * dfHeight;
    padfTerms[7] = dfLong * dfLong;
    padfTerms[8] = dfLat * dfLat;
    padfTerms[9] = dfHeight * dfHeight;

    padfTerms[10] = dfLong * dfLat * dfHeight;
    padfTerms[11] = dfLong * dfLong * dfLong;
    padfTerms[12] = dfLong * dfLat * dfLat;
    padfTerms[13] = dfLong * dfHeight * dfHeight;
    padfTerms[14] = dfLong * dfLong * dfLat;
    padfTerms[15] = dfLat * dfLat * dfLat;
    padfTerms[16] = dfLat * dfHeight * dfHeight;
    padfTerms[17] = dfLong * dfLong * dfHeight;
    padfTerms[18] = dfLat * dfLat * dfHeight;
    padfTerms[19] = dfHeight * dfHeight * dfHeight;
}

/************************************************************************/
/*                            RPCEvaluate()                             */
/************************************************************************/

static double RPCEvaluate( double *padfTerms, double *padfCoefs )

{
    double dfSum = 0.0;
    int i;

    for( i = 0; i < 20; i++ )
        dfSum += padfTerms[i] * padfCoefs[i];

    return dfSum;
}

/************************************************************************/
/*                         RPCTransformPoint()                          */
/************************************************************************/

static void RPCTransformPoint( GDALRPCInfo *psRPC, 
                               double dfLong, double dfLat, double dfHeight, 
                               double *pdfPixel, double *pdfLine )

{
    double dfResultX, dfResultY;
    double adfTerms[20];
   
    RPCComputeTerms( 
        (dfLong   - psRPC->dfLONG_OFF) / psRPC->dfLONG_SCALE, 
        (dfLat    - psRPC->dfLAT_OFF) / psRPC->dfLAT_SCALE, 
        (dfHeight - psRPC->dfHEIGHT_OFF) / psRPC->dfHEIGHT_SCALE,
        adfTerms );
    
    dfResultX = RPCEvaluate( adfTerms, psRPC->adfSAMP_NUM_COEFF )
        / RPCEvaluate( adfTerms, psRPC->adfSAMP_DEN_COEFF );
    
    dfResultY = RPCEvaluate( adfTerms, psRPC->adfLINE_NUM_COEFF )
        / RPCEvaluate( adfTerms, psRPC->adfLINE_DEN_COEFF );
    
    *pdfPixel = dfResultX * psRPC->dfSAMP_SCALE + psRPC->dfSAMP_OFF;
    *pdfLine = dfResultY * psRPC->dfLINE_SCALE + psRPC->dfLINE_OFF;
}

/************************************************************************/
/* ==================================================================== */
/*			     GDALRPCTransformer                         */
/* ==================================================================== */
/************************************************************************/

typedef struct {

    GDALTransformerInfo sTI;

    GDALRPCInfo sRPC;

    double      adfPLToLatLongGeoTransform[6];

    int         bReversed;

    double      dfPixErrThreshold;

    double      dfHeightOffset;

} GDALRPCTransformInfo;

/************************************************************************/
/*                      GDALCreateRPCTransformer()                      */
/************************************************************************/

void *GDALCreateRPCTransformer( GDALRPCInfo *psRPCInfo, int bReversed, 
                                double dfPixErrThreshold,
                                char **papszOptions )

{
    GDALRPCTransformInfo *psTransform;

/* -------------------------------------------------------------------- */
/*      Initialize core info.                                           */
/* -------------------------------------------------------------------- */
    psTransform = (GDALRPCTransformInfo *) 
        CPLCalloc(sizeof(GDALRPCTransformInfo),1);

    memcpy( &(psTransform->sRPC), psRPCInfo, sizeof(GDALRPCInfo) );
    psTransform->bReversed = bReversed;
    psTransform->dfPixErrThreshold = dfPixErrThreshold;
    psTransform->dfHeightOffset = 0.0;

    strcpy( psTransform->sTI.szSignature, "GTI" );
    psTransform->sTI.pszClassName = "GDALRPCTransformer";
    psTransform->sTI.pfnTransform = GDALRPCTransform;
    psTransform->sTI.pfnCleanup = GDALDestroyRPCTransformer;
    psTransform->sTI.pfnSerialize = NULL;

/* -------------------------------------------------------------------- */
/*      Do we have a "average height" that we want to consider all      */
/*      elevations to be relative to?                                   */
/* -------------------------------------------------------------------- */
    const char *pszHeight = CSLFetchNameValue( papszOptions, "RPC_HEIGHT" );
    if( pszHeight != NULL )
        psTransform->dfHeightOffset = CPLAtof(pszHeight);
        
/* -------------------------------------------------------------------- */
/*      Establish a reference point for calcualating an affine          */
/*      geotransform approximate transformation.                        */
/* -------------------------------------------------------------------- */
    double adfGTFromLL[6], dfRefPixel = -1.0, dfRefLine = -1.0;
    double dfRefLong, dfRefLat;

    if( psRPCInfo->dfMIN_LONG != -180 || psRPCInfo->dfMAX_LONG != 180 )
    {
        dfRefLong = (psRPCInfo->dfMIN_LONG + psRPCInfo->dfMAX_LONG) * 0.5;
        dfRefLat  = (psRPCInfo->dfMIN_LAT  + psRPCInfo->dfMAX_LAT ) * 0.5;

        RPCTransformPoint( psRPCInfo, dfRefLong, dfRefLat, 0.0, 
                           &dfRefPixel, &dfRefLine );
    }

    // Try with scale and offset if we don't can't use bounds or
    // the results seem daft. 
    if( dfRefPixel < 0.0 || dfRefLine < 0.0
        || dfRefPixel > 100000 || dfRefLine > 100000 )
    {
        dfRefLong = psRPCInfo->dfLONG_OFF;
        dfRefLat  = psRPCInfo->dfLAT_OFF;

        RPCTransformPoint( psRPCInfo, dfRefLong, dfRefLat, 0.0, 
                           &dfRefPixel, &dfRefLine );
    }

/* -------------------------------------------------------------------- */
/*      Transform nearby locations to establish affine direction        */
/*      vectors.                                                        */
/* -------------------------------------------------------------------- */
    double dfRefPixelDelta, dfRefLineDelta, dfLLDelta = 0.0001;
    
    RPCTransformPoint( psRPCInfo, dfRefLong+dfLLDelta, dfRefLat, 0.0, 
                       &dfRefPixelDelta, &dfRefLineDelta );
    adfGTFromLL[1] = (dfRefPixelDelta - dfRefPixel) / dfLLDelta;
    adfGTFromLL[2] = (dfRefLineDelta - dfRefLine) / dfLLDelta;
    
    RPCTransformPoint( psRPCInfo, dfRefLong, dfRefLat+dfLLDelta, 0.0, 
                       &dfRefPixelDelta, &dfRefLineDelta );
    adfGTFromLL[4] = (dfRefPixelDelta - dfRefPixel) / dfLLDelta;
    adfGTFromLL[5] = (dfRefLineDelta - dfRefLine) / dfLLDelta;

    adfGTFromLL[0] = dfRefPixel 
        - adfGTFromLL[1] * dfRefLong - adfGTFromLL[2] * dfRefLat;
    adfGTFromLL[3] = dfRefLine 
        - adfGTFromLL[4] * dfRefLong - adfGTFromLL[5] * dfRefLat;

    GDALInvGeoTransform( adfGTFromLL, psTransform->adfPLToLatLongGeoTransform);
    
    return psTransform;
}

/************************************************************************/
/*                 GDALDestroyReprojectionTransformer()                 */
/************************************************************************/

void GDALDestroyRPCTransformer( void *pTransformAlg )

{
    CPLFree( pTransformAlg );
}

/************************************************************************/
/*                      RPCInverseTransformPoint()                      */
/************************************************************************/

static void 
RPCInverseTransformPoint( GDALRPCTransformInfo *psTransform,
                          double dfPixel, double dfLine, double dfHeight, 
                          double *pdfLong, double *pdfLat )

{
    double dfResultX, dfResultY;
    int    iIter;
    GDALRPCInfo *psRPC = &(psTransform->sRPC);

/* -------------------------------------------------------------------- */
/*      Compute an initial approximation based on linear                */
/*      interpolation from our reference point.                         */
/* -------------------------------------------------------------------- */
    dfResultX = psTransform->adfPLToLatLongGeoTransform[0]
        + psTransform->adfPLToLatLongGeoTransform[1] * dfPixel
        + psTransform->adfPLToLatLongGeoTransform[2] * dfLine;

    dfResultY = psTransform->adfPLToLatLongGeoTransform[3]
        + psTransform->adfPLToLatLongGeoTransform[4] * dfPixel
        + psTransform->adfPLToLatLongGeoTransform[5] * dfLine;

/* -------------------------------------------------------------------- */
/*      Now iterate, trying to find a closer LL location that will      */
/*      back transform to the indicated pixel and line.                 */
/* -------------------------------------------------------------------- */
    double dfPixelDeltaX, dfPixelDeltaY;

    for( iIter = 0; iIter < 10; iIter++ )
    {
        double dfBackPixel, dfBackLine;

        RPCTransformPoint( psRPC, dfResultX, dfResultY, dfHeight, 
                           &dfBackPixel, &dfBackLine );

        dfPixelDeltaX = dfBackPixel - dfPixel;
        dfPixelDeltaY = dfBackLine - dfLine;

        dfResultX = dfResultX 
            - dfPixelDeltaX * psTransform->adfPLToLatLongGeoTransform[1]
            - dfPixelDeltaY * psTransform->adfPLToLatLongGeoTransform[2];
        dfResultY = dfResultY 
            - dfPixelDeltaX * psTransform->adfPLToLatLongGeoTransform[4]
            - dfPixelDeltaY * psTransform->adfPLToLatLongGeoTransform[5];

        if( ABS(dfPixelDeltaX) < psTransform->dfPixErrThreshold
            && ABS(dfPixelDeltaY) < psTransform->dfPixErrThreshold )
        {
            iIter = -1;
            //CPLDebug( "RPC", "Converged!" );
            break;
        }

    }

    if( iIter != -1 )
        CPLDebug( "RPC", "Iterations %d: Got: %g,%g  Offset=%g,%g", 
                  iIter, 
                  dfResultX, dfResultY,
                  dfPixelDeltaX, dfPixelDeltaY );
    
    *pdfLong = dfResultX;
    *pdfLat = dfResultY;
}

/************************************************************************/
/*                          GDALRPCTransform()                          */
/************************************************************************/

int GDALRPCTransform( void *pTransformArg, int bDstToSrc, 
                      int nPointCount, 
                      double *padfX, double *padfY, double *padfZ,
                      int *panSuccess )

{
    VALIDATE_POINTER1( pTransformArg, "GDALRPCTransform", 0 );

    GDALRPCTransformInfo *psTransform = (GDALRPCTransformInfo *) pTransformArg;
    GDALRPCInfo *psRPC = &(psTransform->sRPC);
    int i;

    if( psTransform->bReversed )
        bDstToSrc = !bDstToSrc;

/* -------------------------------------------------------------------- */
/*      The simple case is transforming from lat/long to pixel/line.    */
/*      Just apply the equations directly.                              */
/* -------------------------------------------------------------------- */
    if( bDstToSrc )
    {
        for( i = 0; i < nPointCount; i++ )
        {
            RPCTransformPoint( psRPC, padfX[i], padfY[i], 
                               padfZ[i] + psTransform->dfHeightOffset, 
                               padfX + i, padfY + i );
            panSuccess[i] = TRUE;
        }

        return TRUE;
    }

/* -------------------------------------------------------------------- */
/*      Compute the inverse (pixel/line/height to lat/long).  This      */
/*      function uses an iterative method from an initial linear        */
/*      approximation.                                                  */
/* -------------------------------------------------------------------- */
    for( i = 0; i < nPointCount; i++ )
    {
        double dfResultX, dfResultY;

        RPCInverseTransformPoint( psTransform, padfX[i], padfY[i], 
                                  padfZ[i] + psTransform->dfHeightOffset,
                                  &dfResultX, &dfResultY );

        padfX[i] = dfResultX;
        padfY[i] = dfResultY;

        panSuccess[i] = TRUE;
    }

    return TRUE;
}
