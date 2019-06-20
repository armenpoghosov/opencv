/*M///////////////////////////////////////////////////////////////////////////////////////
//
//  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
//
//  By downloading, copying, installing or using the software you agree to this license.
//  If you do not agree to this license, do not download, install,
//  copy or use the software.
//
//
//                          License Agreement
//                For Open Source Computer Vision Library
//
// Copyright (C) 2000-2008, Intel Corporation, all rights reserved.
// Copyright (C) 2009, Willow Garage Inc., all rights reserved.
// Copyright (C) 2013, OpenCV Foundation, all rights reserved.
// Third party copyrights are property of their respective owners.
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//   * Redistribution's of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//   * Redistribution's in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//   * The name of the copyright holders may not be used to endorse or promote products
//     derived from this software without specific prior written permission.
//
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall the Intel Corporation or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
//M*/

#include "precomp.hpp"

#include <algorithm>
#include <iterator>
#include <limits>

namespace cv
{

int RANSACUpdateNumIters(double p, double ep, int modelPoints, int maxIters)
{
    if (modelPoints <= 0)
        CV_Error( Error::StsOutOfRange, "the number of model points should be positive" );

    p = MAX(p, 0.);
    p = MIN(p, 1.);
    ep = MAX(ep, 0.);
    ep = MIN(ep, 1.);

    // avoid inf's & nan's
    double num = MAX(1. - p, DBL_MIN);
    double denom = 1. - std::pow(1. - ep, modelPoints);

    if (denom < DBL_MIN)
        return 0;

    num = std::log(num);
    denom = std::log(denom);

    return denom >= 0 || -num >= maxIters * (-denom) ? maxIters : cvRound(num / denom);
}


class RANSACPointSetRegistrator : public PointSetRegistrator
{
public:

    RANSACPointSetRegistrator(Ptr<PointSetRegistrator::Callback> const& _cb = Ptr<PointSetRegistrator::Callback>(),
            int _modelPoints = 0, double _threshold = 0, double _confidence = 0.99, int _maxIters = 1000)
        :
        cb(_cb),
        modelPoints(_modelPoints),
        threshold(_threshold),
        confidence(_confidence),
        maxIters(_maxIters)
    {}

    int findInliers(Mat const& m1, Mat const& m2, Mat const& model, Mat& err, Mat& mask, double thresh) const
    {
        cb->computeError(m1, m2, model, err);
        CV_Assert(err.isContinuous() && err.type() == CV_32F);
        float const* errptr = err.ptr<float>();

        mask.create(err.size(), CV_8U);
        CV_Assert(mask.isContinuous() && mask.type() == CV_8U);
        uchar* maskptr = mask.ptr<uchar>();

        float const t = (float)(thresh * thresh);

        int nz = 0;

        for (int i = 0, n = (int)err.total(); i < n; ++i)
        {
            int f = errptr[i] <= t;
            maskptr[i] = (uchar)f;
            nz += f;
        }

        return nz;
    }

    bool getSubset( const Mat& m1, const Mat& m2, Mat& ms1, Mat& ms2,
        RNG& rng, int maxAttempts=1000 ) const
    {
        cv::AutoBuffer<int> _idx(modelPoints);
        int* idx = _idx.data();

        int d1 = m1.channels() > 1 ? m1.channels() : m1.cols;
        int d2 = m2.channels() > 1 ? m2.channels() : m2.cols;

        int esz1 = (int)m1.elemSize1() * d1;
        int esz2 = (int)m2.elemSize1() * d2;
        CV_Assert((esz1 % sizeof(int)) == 0 && (esz2 % sizeof(int)) == 0);
        esz1 /= sizeof(int);
        esz2 /= sizeof(int);

        int count = m1.checkVector(d1);
        int count2 = m2.checkVector(d2);
        CV_Assert(count >= modelPoints && count == count2);

        int const* m1ptr = m1.ptr<int>();
        int const* m2ptr = m2.ptr<int>();

        ms1.create(modelPoints, 1, CV_MAKETYPE(m1.depth(), d1));
        int* ms1ptr = ms1.ptr<int>();

        ms2.create(modelPoints, 1, CV_MAKETYPE(m2.depth(), d2));
        int* ms2ptr = ms2.ptr<int>();

        for (int iters = 0; iters < maxAttempts; ++iters)
        {
            int i;

            for (i = 0; i < modelPoints; ++i)
            {
                int& idx_i = idx[i];

                for (idx_i = rng.uniform(0, count);
                    std::find(idx, idx + i, idx_i) != idx + i;
                    idx_i = rng.uniform(0, count))

                for (int k = 0; k < esz1; ++k)
                    ms1ptr[i * esz1 + k] = m1ptr[idx_i * esz1 + k];

                for (int k = 0; k < esz2; ++k)
                    ms2ptr[i * esz2 + k] = m2ptr[idx_i * esz2 + k];
            }

            if (cb->checkSubset(ms1, ms2, i))
                return true;
        }

        return false;
    }

    bool run(InputArray _m1, InputArray _m2, OutputArray _model, OutputArray _mask) const CV_OVERRIDE
    {
        CV_Assert(cb != nullptr);
        CV_Assert(confidence > 0 && confidence < 1);

        Mat m1 = _m1.getMat();
        Mat m2 = _m2.getMat();

        int d1 = m1.channels() > 1 ? m1.channels() : m1.cols;
        int d2 = m2.channels() > 1 ? m2.channels() : m2.cols;

        int count = m1.checkVector(d1);
        int count2 = m2.checkVector(d2);
        CV_Assert(count >= 0 && count2 == count);

        if (count < modelPoints)
            return false;

        Mat bestMask0;
        Mat bestMask;

        if (_mask.needed())
        {
            _mask.create(count, 1, CV_8U, -1, true);
            bestMask = _mask.getMat();
            CV_Assert( (bestMask.cols == 1 || bestMask.rows == 1) && (int)bestMask.total() == count );
        }
        else
        {
            bestMask.create(count, 1, CV_8U);
        }

        bestMask0 = bestMask;

        Mat ms1;
        Mat ms2;
        Mat bestModel;

        if (count == modelPoints)
        {
            if (cb->runKernel(m1, m2, bestModel) <= 0)
                return false;

            bestModel.copyTo(_model);
            bestMask.setTo(Scalar::all(1));
            return true;
        }

        Mat err;
        Mat mask;
        Mat model;

        RNG rng((uint64)-1);

        int maxGoodCount = 0;

        for (int iter = 0, niters = MAX(maxIters, 1); iter < niters; ++iter)
        {
            if (count > modelPoints && !getSubset(m1, m2, ms1, ms2, rng, 10000))
            {
                if (iter == 0)
                    return false;

                break;
            }

            int nmodels = cb->runKernel(ms1, ms2, model);
            if (nmodels <= 0)
                continue;

            CV_Assert(model.rows % nmodels == 0);
            Size modelSize(model.cols, model.rows / nmodels);

            for (int i = 0; i < nmodels; ++i)
            {
                Mat model_i = model.rowRange(i * modelSize.height, (i + 1) * modelSize.height);

                int goodCount = findInliers(m1, m2, model_i, err, mask, threshold);

                if (goodCount > MAX(maxGoodCount, modelPoints - 1))
                {
                    std::swap(mask, bestMask);
                    model_i.copyTo(bestModel);

                    maxGoodCount = goodCount;

                    niters = RANSACUpdateNumIters(confidence, (double)(count - goodCount) / count, modelPoints, niters);
                }
            }
        }

        if (maxGoodCount > 0)
        {
            if (bestMask.data != bestMask0.data)
            {
                if (bestMask.size() == bestMask0.size())
                    bestMask.copyTo(bestMask0);
                else
                    transpose(bestMask, bestMask0);
            }

            bestModel.copyTo(_model);
            return true;
        }

        _model.release();
        return false;
    }

    void setCallback(const Ptr<PointSetRegistrator::Callback>& _cb) CV_OVERRIDE
        { cb = _cb; }

    Ptr<PointSetRegistrator::Callback>  cb;
    int                                 modelPoints;
    double                              threshold;
    double                              confidence;
    int                                 maxIters;
};

class LMeDSPointSetRegistrator : public RANSACPointSetRegistrator
{
public:

    LMeDSPointSetRegistrator(Ptr<PointSetRegistrator::Callback> const& _cb = Ptr<PointSetRegistrator::Callback>(),
            int _modelPoints = 0, double _confidence = 0.99, int _maxIters = 1000)
        :
        RANSACPointSetRegistrator(_cb, _modelPoints, 0, _confidence, _maxIters)
    {}

    bool run(InputArray _m1, InputArray _m2, OutputArray _model, OutputArray _mask) const CV_OVERRIDE
    {
        CV_Assert(cb != nullptr);
        CV_Assert(confidence > 0 && confidence < 1);

        double const outlierRatio = 0.45;

        Mat m1 = _m1.getMat();
        Mat m2 = _m2.getMat();

        Mat ms1;
        Mat ms2;
        Mat err;
        Mat errf;
        Mat model;
        Mat bestModel;
        Mat mask;
        Mat mask0;

        int const d1 = m1.channels() > 1 ? m1.channels() : m1.cols;
        int const d2 = m2.channels() > 1 ? m2.channels() : m2.cols;

        int count = m1.checkVector(d1);
        int count2 = m2.checkVector(d2);
        CV_Assert(count >= 0 && count2 == count);

        if (count < modelPoints)
            return false;

        if (_mask.needed())
        {
            _mask.create(count, 1, CV_8U, -1, true);
            mask0 = mask = _mask.getMat();
            CV_Assert( (mask.cols == 1 || mask.rows == 1) && (int)mask.total() == count );
        }

        if (count == modelPoints)
        {
            if (cb->runKernel(m1, m2, bestModel) <= 0)
                return false;

            bestModel.copyTo(_model);
            mask.setTo(Scalar::all(1));
            return true;
        }

        int niters = RANSACUpdateNumIters(confidence, outlierRatio, modelPoints, maxIters);
        niters = MAX(niters, 3);

        double minMedian = DBL_MAX;

        RNG rng((uint64)-1);

        for (int iter = 0; iter < niters; iter++ )
        {
            if (count > modelPoints && !getSubset(m1, m2, ms1, ms2, rng))
            {
                if (iter == 0)
                    return false;
                break;
            }

            int nmodels = cb->runKernel(ms1, ms2, model);
            if (nmodels <= 0)
                continue;

            CV_Assert(model.rows % nmodels == 0);
            Size modelSize(model.cols, model.rows / nmodels);

            for (int i = 0; i < nmodels; ++i)
            {
                Mat model_i = model.rowRange(i * modelSize.height, (i + 1) * modelSize.height);

                cb->computeError(m1, m2, model_i, err);

                if (err.depth() != CV_32F)
                    err.convertTo(errf, CV_32F);
                else
                    errf = err;

                CV_Assert(errf.isContinuous() && errf.type() == CV_32F && (int)errf.total() == count);

                float* errf_ptr = errf.ptr<float>();

                std::nth_element(errf_ptr, errf_ptr + count / 2, errf_ptr + count);

                double const median = errf_ptr[count / 2];

                if (median < minMedian)
                {
                    minMedian = median;
                    model_i.copyTo(bestModel);
                }
            }
        }

        if (minMedian < DBL_MAX)
        {
            double sigma = 2.5 * 1.4826 * (1 + 5. / (count - modelPoints)) * std::sqrt(minMedian);
            sigma = MAX(sigma, 0.001);

            count = findInliers(m1, m2, bestModel, err, mask, sigma);

            if (_mask.needed() && mask0.data != mask.data)
            {
                if (mask0.size() == mask.size())
                    mask.copyTo(mask0);
                else
                    transpose(mask, mask0);
            }

            bestModel.copyTo(_model);
            return count >= modelPoints;
        }

        _model.release();

        return false;
    }

};

Ptr<PointSetRegistrator> createRANSACPointSetRegistrator(Ptr<PointSetRegistrator::Callback> const& _cb,
    int _modelPoints, double _threshold, double _confidence, int _maxIters)
{
    return Ptr<PointSetRegistrator>(
        new RANSACPointSetRegistrator(_cb, _modelPoints, _threshold, _confidence, _maxIters));
}


Ptr<PointSetRegistrator> createLMeDSPointSetRegistrator(Ptr<PointSetRegistrator::Callback> const& _cb,
    int _modelPoints, double _confidence, int _maxIters)
{
    return Ptr<PointSetRegistrator>(
        new LMeDSPointSetRegistrator(_cb, _modelPoints, _confidence, _maxIters));
}


/*
 * Compute
 *  x    a b c   X     t1
 *  y  = d e f * Y  +  t2
 *  z    g h i   Z     t3
 *
 *  - every element in _m1 contains (X,Y,Z), which are called source points
 *  - every element in _m2 contains (x,y,z), which are called destination points
 *  - _model is of size 3x4, which contains
 *     a b c t1
 *     d e f t2
 *     g h i t3
 */
class Affine3DEstimatorCallback : public PointSetRegistrator::Callback
{
public:

    int runKernel(InputArray _m1, InputArray _m2, OutputArray _model) const CV_OVERRIDE
    {
        Mat m1 = _m1.getMat();
        Mat m2 = _m2.getMat();

        Point3f const* from = m1.ptr<Point3f>();
        Point3f const* to = m2.ptr<Point3f>();

        const int N = 12;

        double buf[N * N + N + N];

        Mat A(N, N, CV_64F, &buf[0]);
        Mat B(N, 1, CV_64F, &buf[0] + N * N);
        Mat X(N, 1, CV_64F, &buf[0] + N * N + N);

        double* Adata = A.ptr<double>();
        double* Bdata = B.ptr<double>();

        A = Scalar::all(0);

        for (int i = 0; i < (N / 3); ++i)
        {
            Bdata[i * 3] = to[i].x;
            Bdata[i * 3 + 1] = to[i].y;
            Bdata[i * 3 + 2] = to[i].z;

            double *aptr = Adata + i * 3 * N;

            for (int k = 0; k < 3; ++k)
            {
                aptr[0] = from[i].x;
                aptr[1] = from[i].y;
                aptr[2] = from[i].z;
                aptr[3] = 1.0;
                aptr += 16;
            }
        }

        solve(A, B, X, DECOMP_SVD);

        X.reshape(1, 3).copyTo(_model);

        return 1;
    }

    void computeError(InputArray _m1, InputArray _m2, InputArray _model, OutputArray _err) const CV_OVERRIDE
    {
        Mat m1 = _m1.getMat();
        Point3f const* from = m1.ptr<Point3f>();

        Mat m2 = _m2.getMat();
        Point3f const* to = m2.ptr<Point3f>();

        Mat model = _model.getMat();
        double const* F = model.ptr<double>();

        int count = m1.checkVector(3);
        CV_Assert(count > 0);

        _err.create(count, 1, CV_32F);
        Mat err = _err.getMat();

        float* errptr = err.ptr<float>();

        for (int i = 0; i < count; ++i)
        {
            Point3f const& f = from[i];
            Point3f const& t = to[i];

            double const a = F[0] * f.x + F[1] * f.y + F[2] * f.z + F[ 3] - t.x;
            double const b = F[4] * f.x + F[5] * f.y + F[6] * f.z + F[ 7] - t.y;
            double const c = F[8] * f.x + F[9] * f.y + F[10] * f.z + F[11] - t.z;

            errptr[i] = (float)(a * a + b * b + c * c);
        }
    }

    bool checkSubset(InputArray _ms1, InputArray _ms2, int count) const CV_OVERRIDE
    {
        float const threshold = 0.996f;

        Mat ms1 = _ms1.getMat();
        Mat ms2 = _ms2.getMat();

        for (int inp = 1; inp <= 2; ++inp)
        {
            Mat const* msi = inp == 1 ? &ms1 : &ms2;
            Point3f const* ptr = msi->ptr<Point3f>();

            CV_Assert(count <= msi->rows);

            // check that the i-th selected point does not belong
            // to a line connecting some previously selected points
            for (int j = 0, i = count - 1; j < i; ++j)
            {
                Point3f const d1 = ptr[j] - ptr[i];
                float const n1 = d1.x * d1.x + d1.y * d1.y;

                for(int k = 0; k < j; ++k)
                {
                    Point3f const d2 = ptr[k] - ptr[i];
                    float const denom = (d2.x * d2.x + d2.y * d2.y) * n1;

                    float const num = d1.x * d2.x + d1.y * d2.y;

                    if (num * num > threshold * threshold * denom)
                    {
                        return false;
                    }
                }
            }
        }

        return true;
    }
};

/*
 * Compute
 *  x     a  b   X    c
 *     =       *    +
 *  y     d  e   Y    f
 *
 *  - every element in _m1 contains (X,Y), which are called source points
 *  - every element in _m2 contains (x,y), which are called destination points
 *  - _model is of size 2x3, which contains
 *    a b c
 *    d e f
 */
class Affine2DEstimatorCallback : public PointSetRegistrator::Callback
{
public:

    int runKernel( InputArray _m1, InputArray _m2, OutputArray _model ) const CV_OVERRIDE
    {
        Mat m1 = _m1.getMat(), m2 = _m2.getMat();
        const Point2f* from = m1.ptr<Point2f>();
        const Point2f* to   = m2.ptr<Point2f>();
        _model.create(2, 3, CV_64F);
        Mat M_mat = _model.getMat();
        double *M = M_mat.ptr<double>();

        // we need 3 points to estimate affine transform
        double x1 = from[0].x;
        double y1 = from[0].y;
        double x2 = from[1].x;
        double y2 = from[1].y;
        double x3 = from[2].x;
        double y3 = from[2].y;

        double X1 = to[0].x;
        double Y1 = to[0].y;
        double X2 = to[1].x;
        double Y2 = to[1].y;
        double X3 = to[2].x;
        double Y3 = to[2].y;

        /*
        We want to solve AX = B

            | x1 y1  1  0  0  0 |
            |  0  0  0 x1 y1  1 |
            | x2 y2  1  0  0  0 |
        A = |  0  0  0 x2 y2  1 |
            | x3 y3  1  0  0  0 |
            |  0  0  0 x3 y3  1 |
        B = (X1, Y1, X2, Y2, X3, Y3).t()
        X = (a, b, c, d, e, f).t()

        As the estimate of (a, b, c) only depends on the Xi, and (d, e, f) only
        depends on the Yi, we do the *trick* to solve each one analytically.

        | X1 |   | x1 y1 1 |   | a |
        | X2 | = | x2 y2 1 | * | b |
        | X3 |   | x3 y3 1 |   | c |

        | Y1 |   | x1 y1 1 |   | d |
        | Y2 | = | x2 y2 1 | * | e |
        | Y3 |   | x3 y3 1 |   | f |
        */

        double d = 1. / ( x1*(y2-y3) + x2*(y3-y1) + x3*(y1-y2) );

        M[0] = d * ( X1*(y2-y3) + X2*(y3-y1) + X3*(y1-y2) );
        M[1] = d * ( X1*(x3-x2) + X2*(x1-x3) + X3*(x2-x1) );
        M[2] = d * ( X1*(x2*y3 - x3*y2) + X2*(x3*y1 - x1*y3) + X3*(x1*y2 - x2*y1) );

        M[3] = d * ( Y1*(y2-y3) + Y2*(y3-y1) + Y3*(y1-y2) );
        M[4] = d * ( Y1*(x3-x2) + Y2*(x1-x3) + Y3*(x2-x1) );
        M[5] = d * ( Y1*(x2*y3 - x3*y2) + Y2*(x3*y1 - x1*y3) + Y3*(x1*y2 - x2*y1) );
        return 1;
    }

    void computeError( InputArray _m1, InputArray _m2, InputArray _model, OutputArray _err ) const CV_OVERRIDE
    {
        Mat m1 = _m1.getMat(), m2 = _m2.getMat(), model = _model.getMat();
        const Point2f* from = m1.ptr<Point2f>();
        const Point2f* to   = m2.ptr<Point2f>();
        const double* F = model.ptr<double>();

        int count = m1.checkVector(2);
        CV_Assert( count > 0 );

        _err.create(count, 1, CV_32F);
        Mat err = _err.getMat();
        float* errptr = err.ptr<float>();
        // transform matrix to floats
        float F0 = (float)F[0], F1 = (float)F[1], F2 = (float)F[2];
        float F3 = (float)F[3], F4 = (float)F[4], F5 = (float)F[5];

        for(int i = 0; i < count; i++ )
        {
            const Point2f& f = from[i];
            const Point2f& t = to[i];

            float a = F0*f.x + F1*f.y + F2 - t.x;
            float b = F3*f.x + F4*f.y + F5 - t.y;

            errptr[i] = a*a + b*b;
        }
    }

    bool checkSubset( InputArray _ms1, InputArray _ms2, int count ) const CV_OVERRIDE
    {
        Mat ms1 = _ms1.getMat();
        Mat ms2 = _ms2.getMat();

        // check collinearity and also check that points are too close
        return !haveCollinearPoints(ms1, count) && !haveCollinearPoints(ms2, count);
    }
};

/*
 * Compute
 *  x    c -s    X    t1
 *    =       *     +
 *  y    s  c    Y    t2
 *
 *  - every element in _m1 contains (X,Y), which are called source points
 *  - every element in _m2 contains (x,y), which are called destination points
 *  - _model is of size 2x3, which contains
 *    c  -s  t1
 *    s   c  t2
 */
class AffinePartial2DEstimatorCallback : public Affine2DEstimatorCallback
{
public:
    int runKernel( InputArray _m1, InputArray _m2, OutputArray _model ) const CV_OVERRIDE
    {
        Mat m1 = _m1.getMat(), m2 = _m2.getMat();
        const Point2f* from = m1.ptr<Point2f>();
        const Point2f* to   = m2.ptr<Point2f>();
        _model.create(2, 3, CV_64F);
        Mat M_mat = _model.getMat();
        double *M = M_mat.ptr<double>();

        // we need only 2 points to estimate transform
        double x1 = from[0].x;
        double y1 = from[0].y;
        double x2 = from[1].x;
        double y2 = from[1].y;

        double X1 = to[0].x;
        double Y1 = to[0].y;
        double X2 = to[1].x;
        double Y2 = to[1].y;

        /*
        we are solving AS = B
            | x1 -y1 1 0 |
            | y1  x1 0 1 |
        A = | x2 -y2 1 0 |
            | y2  x2 0 1 |
        B = (X1, Y1, X2, Y2).t()
        we solve that analytically
        */
        double d = 1./((x1-x2)*(x1-x2) + (y1-y2)*(y1-y2));

        // solution vector
        double S0 = d * ( (X1-X2)*(x1-x2) + (Y1-Y2)*(y1-y2) );
        double S1 = d * ( (Y1-Y2)*(x1-x2) - (X1-X2)*(y1-y2) );
        double S2 = d * ( (Y1-Y2)*(x1*y2 - x2*y1) - (X1*y2 - X2*y1)*(y1-y2) - (X1*x2 - X2*x1)*(x1-x2) );
        double S3 = d * (-(X1-X2)*(x1*y2 - x2*y1) - (Y1*x2 - Y2*x1)*(x1-x2) - (Y1*y2 - Y2*y1)*(y1-y2) );

        // set model, rotation part is antisymmetric
        M[0] = M[4] = S0;
        M[1] = -S1;
        M[2] = S2;
        M[3] = S1;
        M[5] = S3;
        return 1;
    }
};

class Affine2DRefineCallback : public LMSolver::Callback
{
public:

    Affine2DRefineCallback(InputArray _src, InputArray _dst)
    {
        src = _src.getMat();
        dst = _dst.getMat();
    }

    bool compute(InputArray _param, OutputArray _err, OutputArray _Jac) const CV_OVERRIDE
    {
        int i, count = src.checkVector(2);
        Mat param = _param.getMat();
        _err.create(count*2, 1, CV_64F);
        Mat err = _err.getMat(), J;

        if( _Jac.needed())
        {
            _Jac.create(count*2, param.rows, CV_64F);
            J = _Jac.getMat();
            CV_Assert( J.isContinuous() && J.cols == 6 );
        }

        const Point2f* M = src.ptr<Point2f>();
        const Point2f* m = dst.ptr<Point2f>();
        const double* h = param.ptr<double>();
        double* errptr = err.ptr<double>();
        double* Jptr = J.data ? J.ptr<double>() : 0;

        for( i = 0; i < count; i++ )
        {
            double Mx = M[i].x, My = M[i].y;
            double xi = h[0]*Mx + h[1]*My + h[2];
            double yi = h[3]*Mx + h[4]*My + h[5];
            errptr[i*2] = xi - m[i].x;
            errptr[i*2+1] = yi - m[i].y;

            /*
            Jacobian should be:
                {x, y, 1, 0, 0, 0}
                {0, 0, 0, x, y, 1}
            */
            if( Jptr )
            {
                Jptr[0] = Mx; Jptr[1] = My; Jptr[2] = 1.;
                Jptr[3] = Jptr[4] = Jptr[5] = 0.;
                Jptr[6] = Jptr[7] = Jptr[8] = 0.;
                Jptr[9] = Mx; Jptr[10] = My; Jptr[11] = 1.;

                Jptr += 6*2;
            }
        }

        return true;
    }

    Mat src, dst;
};

class AffinePartial2DRefineCallback : public LMSolver::Callback
{
public:
    AffinePartial2DRefineCallback(InputArray _src, InputArray _dst)
    {
        src = _src.getMat();
        dst = _dst.getMat();
    }

    bool compute(InputArray _param, OutputArray _err, OutputArray _Jac) const CV_OVERRIDE
    {
        int i, count = src.checkVector(2);
        Mat param = _param.getMat();
        _err.create(count*2, 1, CV_64F);
        Mat err = _err.getMat(), J;
        if( _Jac.needed())
        {
            _Jac.create(count*2, param.rows, CV_64F);
            J = _Jac.getMat();
            CV_Assert( J.isContinuous() && J.cols == 4 );
        }

        const Point2f* M = src.ptr<Point2f>();
        const Point2f* m = dst.ptr<Point2f>();
        const double* h = param.ptr<double>();
        double* errptr = err.ptr<double>();
        double* Jptr = J.data ? J.ptr<double>() : 0;

        for( i = 0; i < count; i++ )
        {
            double Mx = M[i].x, My = M[i].y;
            double xi = h[0]*Mx - h[1]*My + h[2];
            double yi = h[1]*Mx + h[0]*My + h[3];
            errptr[i*2] = xi - m[i].x;
            errptr[i*2+1] = yi - m[i].y;

            /*
            Jacobian should be:
                {x, -y, 1, 0}
                {y,  x, 0, 1}
            */
            if( Jptr )
            {
                Jptr[0] = Mx; Jptr[1] = -My; Jptr[2] = 1.; Jptr[3] = 0.;
                Jptr[4] = My; Jptr[5] =  Mx; Jptr[6] = 0.; Jptr[7] = 1.;

                Jptr += 4*2;
            }
        }

        return true;
    }

    Mat src, dst;
};

int estimateAffine3D(InputArray _from, InputArray _to,
    OutputArray _out, OutputArray _inliers,
    double ransacThreshold, double confidence)
{
    CV_INSTRUMENT_REGION();

    Mat from = _from.getMat();
    Mat to = _to.getMat();

    int count = from.checkVector(3);
    CV_Assert(count >= 0 && to.checkVector(3) == count);

    Mat dFrom, dTo;
    from.convertTo(dFrom, CV_32F);
    to.convertTo(dTo, CV_32F);
    dFrom = dFrom.reshape(3, count);
    dTo = dTo.reshape(3, count);

    const double epsilon = DBL_EPSILON;
    ransacThreshold = ransacThreshold <= 0 ? 3 : ransacThreshold;
    confidence = (confidence < epsilon) ? 0.99 : (confidence > 1 - epsilon) ? 0.99 : confidence;

    return createRANSACPointSetRegistrator(makePtr<Affine3DEstimatorCallback>(), 4, ransacThreshold, confidence)->run(dFrom, dTo, _out, _inliers);
}

Mat estimateAffine2D(InputArray _from, InputArray _to, OutputArray _inliers,
                     const int method, const double ransacReprojThreshold,
                     const size_t maxIters, const double confidence,
                     const size_t refineIters)
{
    Mat from = _from.getMat(), to = _to.getMat();
    int count = from.checkVector(2);
    bool result = false;
    Mat H;

    CV_Assert( count >= 0 && to.checkVector(2) == count );

    if (from.type() != CV_32FC2 || to.type() != CV_32FC2)
    {
        Mat tmp1, tmp2;
        from.convertTo(tmp1, CV_32FC2);
        from = tmp1;
        to.convertTo(tmp2, CV_32FC2);
        to = tmp2;
    }
    else
    {
        // avoid changing of inputs in compressElems() call
        from = from.clone();
        to = to.clone();
    }

    // convert to N x 1 vectors
    from = from.reshape(2, count);
    to = to.reshape(2, count);

    Mat inliers;
    if(_inliers.needed())
    {
        _inliers.create(count, 1, CV_8U, -1, true);
        inliers = _inliers.getMat();
    }

    // run robust method
    Ptr<PointSetRegistrator::Callback> cb = makePtr<Affine2DEstimatorCallback>();
    if( method == RANSAC )
        result = createRANSACPointSetRegistrator(cb, 3, ransacReprojThreshold, confidence, static_cast<int>(maxIters))->run(from, to, H, inliers);
    else if( method == LMEDS )
        result = createLMeDSPointSetRegistrator(cb, 3, confidence, static_cast<int>(maxIters))->run(from, to, H, inliers);
    else
        CV_Error(Error::StsBadArg, "Unknown or unsupported robust estimation method");

    if(result && count > 3 && refineIters)
    {
        // reorder to start with inliers
        compressElems(from.ptr<Point2f>(), inliers.ptr<uchar>(), 1, count);
        int inliers_count = compressElems(to.ptr<Point2f>(), inliers.ptr<uchar>(), 1, count);
        if(inliers_count > 0)
        {
            Mat src = from.rowRange(0, inliers_count);
            Mat dst = to.rowRange(0, inliers_count);
            Mat Hvec = H.reshape(1, 6);
            LMSolver::create(makePtr<Affine2DRefineCallback>(src, dst), static_cast<int>(refineIters))->run(Hvec);
        }
    }

    if (!result)
    {
        H.release();
        if(_inliers.needed())
        {
            inliers = Mat::zeros(count, 1, CV_8U);
            inliers.copyTo(_inliers);
        }
    }

    return H;
}

Mat estimateAffinePartial2D(InputArray _from, InputArray _to, OutputArray _inliers,
                            const int method, const double ransacReprojThreshold,
                            const size_t maxIters, const double confidence,
                            const size_t refineIters)
{
    Mat from = _from.getMat(), to = _to.getMat();
    const int count = from.checkVector(2);
    bool result = false;
    Mat H;

    CV_Assert( count >= 0 && to.checkVector(2) == count );

    if (from.type() != CV_32FC2 || to.type() != CV_32FC2)
    {
        Mat tmp1, tmp2;
        from.convertTo(tmp1, CV_32FC2);
        from = tmp1;
        to.convertTo(tmp2, CV_32FC2);
        to = tmp2;
    }
    else
    {
        // avoid changing of inputs in compressElems() call
        from = from.clone();
        to = to.clone();
    }

    // convert to N x 1 vectors
    from = from.reshape(2, count);
    to = to.reshape(2, count);

    Mat inliers;
    if(_inliers.needed())
    {
        _inliers.create(count, 1, CV_8U, -1, true);
        inliers = _inliers.getMat();
    }

    // run robust estimation
    Ptr<PointSetRegistrator::Callback> cb = makePtr<AffinePartial2DEstimatorCallback>();
    if( method == RANSAC )
        result = createRANSACPointSetRegistrator(cb, 2, ransacReprojThreshold, confidence, static_cast<int>(maxIters))->run(from, to, H, inliers);
    else if( method == LMEDS )
        result = createLMeDSPointSetRegistrator(cb, 2, confidence, static_cast<int>(maxIters))->run(from, to, H, inliers);
    else
        CV_Error(Error::StsBadArg, "Unknown or unsupported robust estimation method");

    if(result && count > 2 && refineIters)
    {
        // reorder to start with inliers
        compressElems(from.ptr<Point2f>(), inliers.ptr<uchar>(), 1, count);
        int inliers_count = compressElems(to.ptr<Point2f>(), inliers.ptr<uchar>(), 1, count);
        if(inliers_count > 0)
        {
            Mat src = from.rowRange(0, inliers_count);
            Mat dst = to.rowRange(0, inliers_count);
            // H is
            //     a -b tx
            //     b  a ty
            // Hvec model for LevMarq is
            //     (a, b, tx, ty)
            double *Hptr = H.ptr<double>();
            double Hvec_buf[4] = {Hptr[0], Hptr[3], Hptr[2], Hptr[5]};
            Mat Hvec (4, 1, CV_64F, Hvec_buf);
            LMSolver::create(makePtr<AffinePartial2DRefineCallback>(src, dst), static_cast<int>(refineIters))->run(Hvec);
            // update H with refined parameters
            Hptr[0] = Hptr[4] = Hvec_buf[0];
            Hptr[1] = -Hvec_buf[1];
            Hptr[2] = Hvec_buf[2];
            Hptr[3] = Hvec_buf[1];
            Hptr[5] = Hvec_buf[3];
        }
    }

    if (!result)
    {
        H.release();
        if(_inliers.needed())
        {
            inliers = Mat::zeros(count, 1, CV_8U);
            inliers.copyTo(_inliers);
        }
    }

    return H;
}

} // namespace cv
