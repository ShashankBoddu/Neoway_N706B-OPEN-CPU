#ifndef TINYML_NN_H
#define TINYML_NN_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TINYML_NUM_STEPS 10
#define TINYML_NUM_CLASSES 2

typedef enum {
    CLASS_NORMAL_AIR = 0,
    CLASS_IP_SOLUTION = 1
} tinyml_class_t;

typedef struct {
    float class_probabilities[TINYML_NUM_CLASSES]; /* [0] = Normal Air, [1] = IP Solution */
    tinyml_class_t predicted_class;
    float confidence;                              /* Probability of predicted class (0.0 to 1.0) */
} tinyml_result_t;

/**
 * @brief Initialize TinyML neural network inference engine.
 */
void tinyml_nn_init(void);

/**
 * @brief Run inference on 10 gas resistance values (Ohms) from HP-354 heater steps.
 * @param gas_res_ohms Array of 10 float values (Ohms).
 * @param result Pointer to output result structure.
 * @return true on success.
 */
bool tinyml_nn_predict(const float gas_res_ohms[TINYML_NUM_STEPS], tinyml_result_t *result);

/**
 * @brief Get human-readable label for a class.
 */
const char* tinyml_class_name(tinyml_class_t cls);

#ifdef __cplusplus
}
#endif

#endif /* TINYML_NN_H */
