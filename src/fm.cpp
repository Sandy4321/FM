#include <Rcpp.h>
#include <omp.h>
#include <cmath>
using namespace Rcpp;
using namespace std;

#define LAYOUT 2

inline float dot_product(const float *x, const float *y, const int N) {
  float res = 0.0;
  #pragma omp simd
  for(size_t i = 0; i < N; i++) {
    res += x[i] * y[i];
  }
  return(res);
}

int omp_thread_count() {
  int n = 0;
  #ifdef _OPENMP
  #pragma omp parallel reduction(+:n)
  #endif
  n += 1;
  return n;
}

inline float clip(float x, float max_val) {
  if(x > max_val) return(max_val);
  if(x < -max_val) return(-max_val);
  return(x);
}

class FMParam {
public:
  FMParam();
  FMParam(float learning_rate,
          int rank,
          float lambda_w, float lambda_v,
          float rmsprop_decay):
    learning_rate(learning_rate),
    rank(rank),
    lambda_w(lambda_w),
    lambda_v(lambda_v) {};

  void init_weights(NumericVector w_R, NumericMatrix v_R) {
    // number of features equal to number of input weights
    this->n_features = w_R.size();

    w.resize(this->n_features);
    grad_w2.resize(this->n_features);

    for(int i = 0; i < n_features; i++) {
      // init single feature weights
      w[i] = w_R[i];
      // gradient square history for single feature weights
      grad_w2[i] = 1;
    }

    if(LAYOUT == 1) {
      v.resize(rank);
      grad_v2.resize(rank);
      for(int k = 0; k < rank; k++) {
        v[k].resize(n_features);
        grad_v2[k].resize(n_features);
        for(int i = 0; i < n_features; i++) {
          v[k][i] = v_R(i,k);
          grad_v2[k][i] = 1;
        }
      }
    } else {
      v.resize(n_features);
      grad_v2.resize(n_features);
      for(int k = 0; k < n_features; k++) {
        // interactions vectors
        v[k].resize(rank);
        // gradient history for interactions weights
        grad_v2[k].resize(rank);

        for(int i = 0; i < rank; i ++) {
          // init with interactions weights
          v[k][i] = v_R(k, i);
          // init gradient square history
          grad_v2[k][i] = 1;
        }
      }
    }


  }
  double learning_rate;

  int n_features;
  int rank;
  double lambda_w;
  double lambda_v;

  float rmsprop_decay;

  vector< float > w;
  vector< vector< float > > v;

  //squared gradients for adaptive learning rate
  vector< vector< float > > grad_v2;
  vector< float > grad_w2;

  float link_function(float x) {
    return(1.0 / ( 1.0 + exp(-x)));
  }
  float loss(float pred, float actual) {
    return(-log( this->link_function(pred * actual) ));
  }
  List dump() {
    NumericVector w_dump(n_features);
    NumericMatrix v_dump(n_features, rank);
    for(int i = 0; i < n_features; i++) {
      w_dump[i] = w[i];
      for(int j = 0; j < rank; j++)
        if(LAYOUT == 1)
          v_dump(i, j) = v[j][i];
        else
          v_dump(i, j) = v[i][j];
    }
    return(List::create(_["w"] = w_dump, _["v"] = v_dump));
  }
};

class FMModel {
public:
  FMModel(FMParam *params): params(params) {};
  FMParam *params;

  float fm_predict_internal(int *nnz_index, const vector<float> &nnz_value, int offset_start, int offset_end) {
    float res = 0.0;
    // add linear terms
    for(int j = offset_start; j < offset_end; j++) {
      int feature_index = nnz_index[j];
      res += this->params->w[feature_index] * (float)nnz_value[j];
    }
    float res_pair_interactions = 0.0;
    // add interactions
    for(int f = 0; f < this->params->rank; f++) {
      float s1 = 0.0;
      //#pragma omp simd
      for(int j = offset_start; j < offset_end; j++) {
        int feature_index = nnz_index[j];
        if(LAYOUT == 1)
          s1 += this->params->v[f][feature_index] * (float)nnz_value[j];
        else
          s1 += this->params->v[feature_index][f] * (float)nnz_value[j];
      }
      s1 = s1 * s1;
      float s2 = 0;
      //#pragma omp simd
      for(int j = offset_start; j < offset_end; j++) {
        int feature_index = nnz_index[j];
        float prod;
        if(LAYOUT == 1)
          prod = this->params->v[f][feature_index] * (float)nnz_value[j];
        else
          prod = this->params->v[feature_index][f] * (float)nnz_value[j];
        s2 += prod * prod;
      }
      res_pair_interactions += s1 - s2;
    }
    return(res + 0.5 * res_pair_interactions);
  }

  NumericVector fit_predict(const S4 &m, const NumericVector &y_R, int nthread = 0, int do_update = 1, float rmsprop_decay = 0.9) {
    int nth = omp_thread_count();
    const double *y = y_R.begin();
    // override if user manually specified number of threads
    if(nthread > 0)
      nth = nthread;

    IntegerVector dims = m.slot("Dim");
    // numer of sample in current mini-batch
    int N = dims[0];
    // allocate space for result
    NumericVector y_hat_R(N);
    // get pointers to not touch R API
    double *y_hat = y_hat_R.begin();

    // just to extract vectors from S4
    // and get pointers to data - we can't touch R API in threads, so will use raw pointers
    IntegerVector PP = m.slot("p");
    int *P = PP.begin();
    IntegerVector JJ = m.slot("j");
    int *J = JJ.begin();
    NumericVector XX = m.slot("x");
    //double *X = XX.begin();
    vector<float> X(XX.size());
    for(int i = 0; i < XX.size(); i++)
      X[i] = XX[i];

    #ifdef _OPENMP
    #pragma omp parallel for num_threads(nth)
    #endif
    for(int i = 0; i < N; i++) {
      size_t p1 = P[i];
      size_t p2 = P[i + 1];
      float y_hat_raw = this->fm_predict_internal(J, X, p1, p2);
      // prediction
      y_hat[i] = this->params->link_function(y_hat_raw);
      // fitting
      if(do_update) {
        // first part of d_L/d_theta -  intependent of parameters theta
        float dL = (this->params->link_function(y_hat_raw * y[i]) - 1) * y[i];

        for( int p = p1; p < p2; p++) {

          int   feature_index  = J[p];
          float feature_value = X[p];

          float grad_w = clip(feature_value * dL + 2 * this->params->lambda_w, 100);
          this->params->w[feature_index] -= this->params->learning_rate * grad_w / sqrt(this->params->grad_w2[feature_index]);

          // this->params->grad_w2[feature_index] =
          //   rmsprop_decay * this->params->grad_w2[feature_index] +
          //   (1 - rmsprop_decay) * grad_w * grad_w;

          // update sum gradient squre
          this->params->grad_w2[feature_index] += grad_w * grad_w;

          // pairwise interactions
          // for(int f = 0; f < this->params->rank; f++) {
          //   float grad_v = 0;
          //   int len = p2 - p1;
          //   for(int j = p1; j < p2; j++) {
          //     int feature_index_2 = J[j];
          //     float feature_value_2 = X[j];
          //     grad_v += this->params->v[feature_index_2][f] * feature_value_2;
          //   }
          // }

          for(int f = 0; f < this->params->rank; f++) {

            // float grad_v = dot_product(&this->params->v[J[p1]][0], &X[p1], p2 - p1);

            float grad_v = 0;
            //#pragma omp simd
            for(int j = p1; j < p2; j++) {
              int feature_index_2 = J[j];
              float feature_value_2 = X[j];
              if(LAYOUT == 1)
                grad_v += this->params->v[f][feature_index_2] * feature_value_2;
              else
                grad_v += this->params->v[feature_index_2][f] * feature_value_2;
            }


            if(LAYOUT == 1)
              grad_v = grad_v * feature_value - this->params->v[f][feature_index] * feature_value * feature_value;
            else
              grad_v = grad_v * feature_value - this->params->v[feature_index][f] * feature_value * feature_value;

            grad_v = clip(grad_v * dL, 100);

            float reg;
            if(LAYOUT == 1)
              reg = 2 * this->params->v[f][feature_index] * this->params->lambda_v;
            else
              reg = 2 * this->params->v[feature_index][f] * this->params->lambda_v;

            grad_v = grad_v + reg;
            if(LAYOUT == 1)
              this->params->v[f][feature_index] -= this->params->learning_rate * grad_v / sqrt(this->params->grad_v2[f][feature_index]);
            else
              this->params->v[feature_index][f] -= this->params->learning_rate * grad_v / sqrt(this->params->grad_v2[feature_index][f]);


            // update sum gradient squre
            if(LAYOUT == 1)
              this->params->grad_v2[f][feature_index] += grad_v * grad_v;
            else
              this->params->grad_v2[feature_index][f] += grad_v * grad_v;

            // this->params->grad_v2[feature_index][f] =
            //   rmsprop_decay * this->params->grad_v2[feature_index][f] +
            //   (1 - rmsprop_decay) * grad_v * grad_v;
          }
        }
      }
    }
    return(y_hat_R);
  }
};

// [[Rcpp::export]]
SEXP fm_create_param(double learning_rate,
                     int rank,
                     double lambda_w, double lambda_v,
                     float rmsprop_decay = 0.9) {
  FMParam * param = new FMParam(learning_rate, rank,
                                lambda_w, lambda_v, rmsprop_decay);
  XPtr< FMParam> ptr(param, true);
  return ptr;
}

// [[Rcpp::export]]
void fm_init_weights(SEXP ptr, const NumericVector &w_R, const NumericMatrix &v_R) {
  Rcpp::XPtr<FMParam> params(ptr);
  params->init_weights(w_R, v_R);
}

// [[Rcpp::export]]
NumericVector fm_partial_fit(SEXP ptr, const S4 &X, const NumericVector &y, int nthread = 1, int do_update = 1) {
  Rcpp::XPtr<FMParam> params(ptr);
  FMModel model(params);
  return(model.fit_predict(X, y, nthread, do_update));
}

// [[Rcpp::export]]
List fm_dump(SEXP ptr) {
  Rcpp::XPtr<FMParam> params(ptr);
  FMModel model(params);
  return(model.params->dump());
}