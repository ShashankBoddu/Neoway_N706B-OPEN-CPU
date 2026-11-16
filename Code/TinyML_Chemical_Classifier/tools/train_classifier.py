#!/usr/bin/env python3
"""
TinyML Model Trainer & C Header Exporter for Bosch BME688
Runs on PC/Workstation to process CSV datasets collected by Neoway N706B OpenCPU.

Outputs:
1. Trained Scikit-Learn Model (Random Forest or Decision Tree)
2. Standalone C header file (bme688_model.h) ready for deployment back onto N706B OpenCPU
"""

import os
import glob
import argparse
import numpy as np
import pandas as pd
from sklearn.model_selection import train_test_split
from sklearn.ensemble import RandomForestClassifier
from sklearn.tree import DecisionTreeClassifier
from sklearn.metrics import classification_report, confusion_matrix

FEATURE_COLS = [
    'temp_c', 'humidity_pct', 'pressure_hpa',
    'gas_res_0_ohm', 'gas_res_1_ohm', 'gas_res_2_ohm', 'gas_res_3_ohm', 'gas_res_4_ohm',
    'gas_res_5_ohm', 'gas_res_6_ohm', 'gas_res_7_ohm', 'gas_res_8_ohm', 'gas_res_9_ohm'
]

def load_datasets(data_dir):
    pattern = os.path.join(data_dir, "dataset_*.csv")
    csv_files = glob.glob(pattern)
    if not csv_files:
        # Check current directory as fallback
        csv_files = glob.glob("*.csv")
    
    if not csv_files:
        raise FileNotFoundError(f"No dataset_*.csv files found in '{data_dir}'!")

    dfs = []
    print(f"Loading {len(csv_files)} CSV files from {data_dir}...")
    for f in csv_files:
        df = pd.read_csv(f)
        print(f"  * {os.path.basename(f)}: {len(df)} samples, label: {df['label'].iloc[0] if 'label' in df else 'N/A'}")
        dfs.append(df)

    data = pd.concat(dfs, ignore_index=True)
    return data

def preprocess_features(df):
    X = df[FEATURE_COLS].copy()
    
    # Apply log10 scaling on high dynamic-range gas resistance values
    for i in range(10):
        col = f'gas_res_{i}_ohm'
        X[col] = np.log10(np.maximum(X[col], 1.0))
    
    y = df['label'].values
    return X, y

def export_c_header(clf, class_names, output_header_path):
    """
    Exports a trained Decision Tree Classifier to a standalone C header file
    with zero external dependencies (pure if-else decision path).
    """
    tree = clf.tree_
    
    with open(output_header_path, 'w') as f:
        f.write("/**\n")
        f.write(" * Automatically generated BME688 TinyML Classifier Model\n")
        f.write(" * Target: Neoway N706B OpenCPU (ARM Cortex-R5)\n")
        f.write(" */\n\n")
        f.write("#ifndef __BME688_MODEL_H__\n")
        f.write("#define __BME688_MODEL_H__\n\n")
        f.write("#include <stdint.h>\n")
        f.write("#include <math.h>\n\n")
        
        # Enums
        f.write("typedef enum {\n")
        for i, name in enumerate(class_names):
            f.write(f"    CLASS_{name.upper()} = {i},\n")
        f.write(f"    CLASS_UNKNOWN = {len(class_names)}\n")
        f.write("} bme688_class_e;\n\n")

        # Class string table
        f.write("static const char * const bme688_class_names[] = {\n")
        for name in class_names:
            f.write(f'    "{name}",\n')
        f.write('    "UNKNOWN"\n')
        f.write("};\n\n")

        # Inference function signature
        f.write("static inline bme688_class_e bme688_predict(\n")
        f.write("    float temp_c, float humidity_pct, float pressure_hpa,\n")
        f.write("    const float gas_res_ohms[10])\n")
        f.write("{\n")
        f.write("    /* Feature transformation: log10 of gas resistances */\n")
        f.write("    float feats[13];\n")
        f.write("    feats[0] = temp_c;\n")
        f.write("    feats[1] = humidity_pct;\n")
        f.write("    feats[2] = pressure_hpa;\n")
        f.write("    for (int i = 0; i < 10; i++) {\n")
        f.write("        feats[3 + i] = (gas_res_ohms[i] > 1.0f) ? log10f(gas_res_ohms[i]) : 0.0f;\n")
        f.write("    }\n\n")

        # Tree recursion
        def recurse_node(node_id, depth):
            indent = "    " * depth
            if tree.feature[node_id] != -2: # Not leaf
                feat_idx = tree.feature[node_id]
                threshold = tree.threshold[node_id]
                f.write(f"{indent}if (feats[{feat_idx}] <= {threshold:.6f}f) {{\n")
                recurse_node(tree.children_left[node_id], depth + 1)
                f.write(f"{indent}}} else {{\n")
                recurse_node(tree.children_right[node_id], depth + 1)
                f.write(f"{indent}}}\n")
            else: # Leaf
                class_idx = np.argmax(tree.value[node_id])
                f.write(f"{indent}return CLASS_{class_names[class_idx].upper()};\n")

        recurse_node(0, 1)
        f.write("}\n\n")
        f.write("#endif /* __BME688_MODEL_H__ */\n")

    print(f"C inference model generated at: {output_header_path}")

def main():
    parser = argparse.ArgumentParser(description="Train TinyML gas classifier on BME688 SD card datasets.")
    parser.add_argument("--data-dir", default=".", help="Directory containing dataset_*.csv files")
    parser.add_argument("--output-header", default="bme688_model.h", help="Target C header file path")
    parser.add_argument("--max-depth", type=int, default=6, help="Maximum decision tree depth (for embedded fit)")
    args = parser.parse_args()

    df = load_datasets(args.data_dir)
    X, y = preprocess_features(df)
    
    class_names = sorted(list(np.unique(y)))
    print(f"Classes found ({len(class_names)}): {class_names}")

    X_train, X_test, y_train, y_test = train_test_split(X, y, test_size=0.25, random_state=42, stratify=y)
    
    # Train Decision Tree
    clf = DecisionTreeClassifier(max_depth=args.max_depth, min_samples_leaf=3, random_state=42)
    clf.fit(X_train, y_train)

    y_pred = clf.predict(X_test)
    print("\n================ Classification Report ================")
    print(classification_report(y_test, y_pred, target_names=class_names))
    print("Confusion Matrix:")
    print(confusion_matrix(y_test, y_pred))

    # Export C header
    export_c_header(clf, class_names, args.output_header)

if __name__ == "__main__":
    main()
