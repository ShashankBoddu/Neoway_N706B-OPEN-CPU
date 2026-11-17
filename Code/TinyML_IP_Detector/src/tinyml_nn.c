/*
 * ==============================================================================
 * TinyML Neural Network Inference Engine
 * Trained by Bosch Sensortec BME AI-Studio v3.2.1
 * Target: SmartElex BME688 AI Gas Scanner (HP-354 Multi-Heater Profile)
 * Architecture: Dense(10, ReLU) -> Dense(10, ReLU) -> Dense(2, Softmax)
 * Classes: [0] NORMAL AIR, [1] IP SOLUTION
 * ==============================================================================
 */

#include "tinyml_nn.h"
#include <math.h>
#include <string.h>

/* Normalization parameters (Mean and StdDev per HP-354 step) */
static const float s_norm_means[TINYML_NUM_STEPS] = {
    8.97120234e+04f, 2.90926775e+06f, 2.48173500e+06f, 2.06828675e+06f, 2.95315312e+05f,
    2.73304656e+05f, 2.55839578e+05f, 7.86700938e+04f, 8.77918828e+04f, 9.23591641e+04f
};

static const float s_norm_stds[TINYML_NUM_STEPS] = {
    6.52675938e+04f, 2.37737475e+06f, 2.06007338e+06f, 1.74828250e+06f, 2.38298656e+05f,
    2.19916438e+05f, 2.05277859e+05f, 5.62836328e+04f, 6.35371875e+04f, 6.72589375e+04f
};

/* Layer 1: Dense(10, 10, relu) */
static const float s_w1[10][10] = {
    { 4.44317222e-01f, 3.86307895e-01f, -6.15575373e-01f, 1.52455689e-02f, 2.71043420e-01f, -1.42604500e-01f, 3.83256227e-02f, -2.21330095e-02f, -7.30631724e-02f, 2.29492754e-01f },
    { 1.41383752e-01f, 4.77743924e-01f, -4.64360118e-01f, -5.81292689e-01f, 6.17896020e-01f, 3.01590804e-02f, -3.60363573e-02f, -4.38853413e-01f, -2.76849091e-01f, -9.19998139e-02f },
    { 2.91413337e-01f, 2.76462227e-01f, 2.01781448e-02f, -3.34477812e-01f, -2.28601322e-02f, -2.08113402e-01f, 1.95971593e-01f, 1.29909024e-01f, 2.06166431e-01f, -4.43097442e-01f },
    { -4.60563451e-02f, -4.07354683e-01f, -2.02385962e-01f, 2.68817663e-01f, 2.20699951e-01f, 2.81050891e-01f, -1.78368121e-01f, -9.07060280e-02f, -5.00974476e-01f, 6.40145466e-02f },
    { 3.38857695e-02f, 4.08716440e-01f, -1.99932039e-01f, -1.03243217e-01f, 2.93516964e-01f, 2.18024403e-01f, -4.14414287e-01f, -3.28540236e-01f, -4.09776671e-03f, -1.85336486e-01f },
    { -9.84052345e-02f, 4.86672282e-01f, -3.82073247e-03f, -3.61251265e-01f, -5.87789565e-02f, -1.86579823e-01f, 4.96620424e-02f, -1.28795415e-01f, -2.76988178e-01f, 1.10372946e-01f },
    { 1.48599669e-01f, -2.94836670e-01f, -2.89063334e-01f, -5.75434148e-01f, 5.78361809e-01f, 1.27148494e-01f, 4.69121821e-02f, -2.52604485e-01f, -6.20703697e-01f, 9.10999253e-03f },
    { 3.16671357e-02f, 6.99548662e-01f, -6.99103355e-01f, -6.70422614e-01f, 4.03585523e-01f, -5.14645636e-01f, 7.07989782e-02f, -1.62079081e-01f, -3.19896303e-02f, -6.88715100e-01f },
    { -3.11234593e-01f, 7.73759365e-01f, -5.23302197e-01f, -2.69295126e-01f, 2.42920294e-01f, 4.27641362e-01f, 1.11181764e-02f, 2.80740380e-01f, -1.07222080e-01f, 1.95277944e-01f },
    { 6.06208980e-01f, 3.51151556e-01f, -1.71052992e-01f, -4.18976769e-02f, 1.51039690e-01f, -2.07674578e-02f, -1.71237424e-01f, -2.87517756e-01f, -3.77143592e-01f, -7.59153306e-01f },
};

static const float s_b1[10] = {
    2.41576850e-01f, 3.81933391e-01f, -4.23347056e-01f, -2.64942110e-01f, 3.87847066e-01f,
    -7.09361807e-02f, -1.21399678e-01f, -3.19294453e-01f, 2.27742419e-01f, -3.21332574e-01f
};

/* Layer 2: Dense(10, 10, relu) */
static const float s_w2[10][10] = {
    { 2.05210119e-01f, -1.42306268e-01f, -3.20542812e-01f, 3.66261840e-01f, -3.80445900e-03f, 6.31278276e-01f, -2.07097188e-01f, -5.99894524e-01f, 1.38540596e-01f, -1.52240306e-01f },
    { 5.10216272e-03f, 8.78103673e-02f, -3.27855706e-01f, 3.02759439e-01f, -4.59444553e-01f, 2.35740826e-01f, 8.55458900e-03f, 1.21868350e-01f, 4.73100394e-01f, 4.05364394e-01f },
    { 4.99101847e-01f, 2.45362386e-01f, -6.35199063e-03f, -5.60873270e-01f, -4.34768885e-01f, -5.03991991e-02f, -2.90497541e-01f, -3.16329539e-01f, 1.10272393e-01f, 6.09705865e-01f },
    { 7.11205065e-01f, 7.06940174e-01f, 8.53016675e-02f, 1.63997412e-01f, -3.11269462e-01f, 3.40807170e-01f, 2.92809665e-01f, -4.77703959e-01f, -8.74089971e-02f, 1.98860187e-02f },
    { -5.01457565e-02f, 1.56877637e-02f, -2.65786171e-01f, 7.31497467e-01f, 1.14214450e-01f, 7.38488793e-01f, -8.77769664e-02f, 1.45724580e-01f, 5.43731451e-01f, -4.88978297e-01f },
    { -1.84072182e-01f, -3.07686299e-01f, 3.05558473e-01f, 2.66637325e-01f, 2.45631471e-01f, -4.45869595e-01f, -7.67829195e-02f, -2.89140314e-01f, 3.46140191e-02f, 2.19996214e-01f },
    { 5.32118797e-01f, 2.84845352e-01f, -2.16989323e-01f, -2.52938624e-02f, -1.31121147e-02f, -1.78331777e-01f, 6.38111606e-02f, 5.23625076e-01f, 7.74659887e-02f, 3.89529690e-02f },
    { 3.23844761e-01f, 6.94176406e-02f, 1.84980765e-01f, -2.20628753e-02f, -4.53451127e-01f, 5.50603447e-03f, 4.75078374e-02f, 4.39926833e-01f, -3.72126043e-01f, 4.51588839e-01f },
    { 6.51827276e-01f, 1.77629933e-01f, -4.59237099e-01f, 3.03469598e-01f, 3.06185842e-01f, 1.53532803e-01f, -3.26107562e-01f, -1.76982716e-01f, 3.97465974e-01f, 4.91546273e-01f },
    { -7.50581920e-02f, 4.13353592e-01f, 5.01290560e-01f, 1.45646751e-01f, -2.30345782e-02f, -3.81778628e-01f, 2.27729961e-01f, 2.25283638e-01f, -3.56816620e-01f, -6.12622425e-02f },
};

static const float s_b2[10] = {
    -2.24144429e-01f, -2.05631599e-01f, -2.18247529e-02f, 4.49647874e-01f, -1.45563185e-01f,
    2.22551361e-01f, 0.00000000e+00f, -5.34501225e-02f, 3.29335928e-01f, -3.92881066e-01f
};

/* Layer 3: Dense(10, 2, softmax) */
static const float s_w3[10][2] = {
    { 5.56699894e-02f, 5.75465918e-01f },
    { 6.84207007e-02f, 7.60259032e-01f },
    { -2.07414791e-01f, -3.66631001e-01f },
    { 5.27619541e-01f, -2.98055142e-01f },
    { -3.85238230e-01f, 5.89981794e-01f },
    { 1.84637591e-01f, -3.04297388e-01f },
    { -5.45985043e-01f, 3.07215154e-01f },
    { -3.04533213e-01f, 4.62362319e-01f },
    { 4.67217416e-01f, -8.86261702e-01f },
    { -6.77177608e-02f, 3.75486404e-01f },
};

static const float s_b3[2] = {
    2.63345778e-01f, -2.63345778e-01f
};

static inline float relu(float x)
{
    return (x > 0.0f) ? x : 0.0f;
}

void tinyml_nn_init(void)
{
    /* Nothing dynamic to allocate - zero RAM footprint */
}

const char* tinyml_class_name(tinyml_class_t cls)
{
    switch (cls) {
        case CLASS_NORMAL_AIR:  return "NORMAL AIR";
        case CLASS_IP_SOLUTION: return "IP SOLUTION";
        default:                return "UNKNOWN";
    }
}

/* Fast self-contained exponential approximation for Softmax */
static inline float tinyml_expf(float x)
{
    if (x < -20.0f) return 0.0f;
    if (x > 20.0f) x = 20.0f;
    /* Range scaling: e^x = (e^(x/16))^16 */
    x *= (1.0f / 16.0f);
    float term = 1.0f + x * (1.0f + x * (0.5f + x * (0.16666667f + x * (0.04166667f + x * 0.00833333f))));
    term *= term; /* ^2 */
    term *= term; /* ^4 */
    term *= term; /* ^8 */
    term *= term; /* ^16 */
    return term;
}

bool tinyml_nn_predict(const float gas_res_ohms[TINYML_NUM_STEPS], tinyml_result_t *result)
{
    if (!gas_res_ohms || !result) return false;

    float norm_input[10];
    float h1[10];
    float h2[10];
    float logits[2];

    /* 1. Feature Standardization (Z-score normalization) */
    for (int i = 0; i < 10; i++) {
        float std = (s_norm_stds[i] > 1e-6f) ? s_norm_stds[i] : 1.0f;
        norm_input[i] = (gas_res_ohms[i] - s_norm_means[i]) / std;
    }

    /* 2. Layer 1 Forward Pass: h1 = ReLU(norm_input * w1 + b1) */
    for (int j = 0; j < 10; j++) {
        float sum = s_b1[j];
        for (int i = 0; i < 10; i++) {
            sum += norm_input[i] * s_w1[i][j];
        }
        h1[j] = relu(sum);
    }

    /* 3. Layer 2 Forward Pass: h2 = ReLU(h1 * w2 + b2) */
    for (int k = 0; k < 10; k++) {
        float sum = s_b2[k];
        for (int j = 0; j < 10; j++) {
            sum += h1[j] * s_w2[j][k];
        }
        h2[k] = relu(sum);
    }

    /* 4. Output Layer: logits = h2 * w3 + b3 */
    for (int m = 0; m < 2; m++) {
        float sum = s_b3[m];
        for (int k = 0; k < 10; k++) {
            sum += h2[k] * s_w3[k][m];
        }
        logits[m] = sum;
    }

    /* 5. Numerically stable Softmax */
    float max_logit = (logits[0] > logits[1]) ? logits[0] : logits[1];
    float exp0 = tinyml_expf(logits[0] - max_logit);
    float exp1 = tinyml_expf(logits[1] - max_logit);
    float sum_exp = exp0 + exp1;

    result->class_probabilities[0] = exp0 / sum_exp; /* NORMAL AIR */
    result->class_probabilities[1] = exp1 / sum_exp; /* IP SOLUTION */

    if (result->class_probabilities[1] > result->class_probabilities[0]) {
        result->predicted_class = CLASS_IP_SOLUTION;
        result->confidence = result->class_probabilities[1];
    } else {
        result->predicted_class = CLASS_NORMAL_AIR;
        result->confidence = result->class_probabilities[0];
    }

    return true;
}
