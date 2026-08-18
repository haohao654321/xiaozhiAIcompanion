/**
 * @file face_recognition.cpp
 * @brief P7a 人脸识别模块实现
 *
 * 检测+识别流程 (官方 esp-face 示例同款):
 *   detector.infer(rgb, {h,w,3}) → results[].keypoint (5点关键点)
 *   recognizer.recognize(tensor, keypoint) → face_info_t {id, name, similarity}
 *   recognizer.enroll_id(tensor, keypoint, name, update_flash)
 *
 * 关键注意:
 *   - Tensor 用 set_element(ptr, false) 零拷贝包装, 不接管 rgb 所有权
 *   - detector 的 results 是内部复用 list, 用前拷贝 keypoint
 *   - 注册时 update_flash=true 直接落 "fr" 分区 (断电不丢)
 *   - 检测器/识别器在 begin() 里惰性 new — PSRAM 初始化后再构造, 无全局构造时序风险
 */
#include "face_recognition.h"
#include <string>

// 注册质量门槛: 检测分数 + 人脸框最小尺寸 (320x240 画面里脸太小 → 112x112 对齐质量差)
#define FACE_ENROLL_MIN_SCORE  0.4f
#define FACE_ENROLL_MIN_BOX    60

// ── 辅助: 从检测框估算 5 个面部关键点 ──
// MSR01 纯检测模型不输出关键点, 但 align_face 需要 5 点 (10 ints).
// 用框内比例估算: 左眼→右眼→鼻尖→左嘴角→右嘴角
static std::vector<int> landmarksFromBox(const std::vector<int>& box) {
    int x1 = box[0], y1 = box[1], x2 = box[2], y2 = box[3];
    int w = x2 - x1, h = y2 - y1;
    return {
        x1 + (int)(w * 0.30f), y1 + (int)(h * 0.30f),  // 左眼
        x1 + (int)(w * 0.70f), y1 + (int)(h * 0.30f),  // 右眼
        x1 + (int)(w * 0.50f), y1 + (int)(h * 0.45f),  // 鼻尖
        x1 + (int)(w * 0.30f), y1 + (int)(h * 0.70f),  // 左嘴角
        x1 + (int)(w * 0.70f), y1 + (int)(h * 0.70f),  // 右嘴角
    };
}

void FaceRecognition::begin() {
    if (_ready) return;

    // 1. 惰性构造 (与 esp-face-test 验证过一致的参数: 阈值 0.3 提高小脸/弱光检出率)
    //    S8 量化识别器 — Flash 减半 (~1.45MB), 家庭 3 人注册场景精度足够
    if (!_detector)  _detector  = new HumanFaceDetectMSR01(0.3f, 0.3f, 10, 0.3f);
    if (!_recognizer) _recognizer = new FaceRecognition112V1S8();
    if (!_detector || !_recognizer) {
        Serial.println("[FACE] FAIL: model alloc");
        return;
    }

    // 2. flash 分区 (人脸 ID 持久化)
    int partOk = _recognizer->set_partition(ESP_PARTITION_TYPE_DATA,
                                            ESP_PARTITION_SUBTYPE_ANY,
                                            FACE_PARTITION_LABEL);
    if (partOk == 1) {
        int n = _recognizer->set_ids_from_flash();
        if (n >= 0) {
            Serial.printf("[FACE] flash '%s' OK, loaded %d id(s)\n", FACE_PARTITION_LABEL, n);
        } else if (n == -1) {
            Serial.printf("[FACE] flash '%s' empty/fresh (code=-1) — persist on first enroll\n",
                          FACE_PARTITION_LABEL);
        } else {
            Serial.printf("[FACE] flash '%s' warn (code=%d)\n", FACE_PARTITION_LABEL, n);
        }
    } else {
        Serial.printf("[FACE] WARN: no '%s' partition — IDs won't persist across reboot\n",
                      FACE_PARTITION_LABEL);
    }

    _ready = true;
    Serial.printf("[FACE] ready: threshold=%.2f, enrolled=%d\n",
                  _recognizer->get_thresh(), _recognizer->get_enrolled_id_num());
}

int FaceRecognition::enrolledCount() const {
    return (_recognizer) ? _recognizer->get_enrolled_id_num() : 0;
}

// ── 检测 + 识别 ──
int FaceRecognition::process(uint8_t* rgb, int w, int h,
                             int& outId, String& outName, float& outScore) {
    if (!_ready || !_detector || !_recognizer || !rgb || w <= 0 || h <= 0) return -1;

    // 1. 检测
    std::list<dl::detect::result_t>& results = _detector->infer<uint8_t>(rgb, {h, w, 3});
    // 调试: 打印检测结果数量及最高分 (首次运行即可看到)
    if (!results.empty()) {
        float bestScore = results.begin()->score;
        for (auto it = results.begin(); it != results.end(); ++it)
            if (it->score > bestScore) bestScore = it->score;
        Serial.printf("[FACE-DBG] detect: %d results, best score=%.3f\n",
                      (int)results.size(), bestScore);
    } else {
        Serial.println("[FACE-DBG] detect: 0 results");
    }
    if (results.empty()) return 1;                       // 无人脸

    // 取分数最高的一帧 (检测器一般已排序, 双保险)
    auto best = results.begin();
    for (auto it = results.begin(); it != results.end(); ++it) {
        if (it->score > best->score) best = it;
    }

    // 2. 关键点: MSR01 不输出关键点, 从框估算
    std::vector<int> landmarks = best->keypoint;
    if (landmarks.size() < 10) {
        Serial.printf("[FACE] process: keypoint too small (%d), est from box\n", (int)landmarks.size());
        landmarks = landmarksFromBox(best->box);
    }
    dl::Tensor<uint8_t> in;
    in.set_element(rgb, false).set_shape({h, w, 3});
    face_info_t info = _recognizer->recognize(in, landmarks);

    outId    = info.id;
    outName  = String(info.name.c_str());
    outScore = info.similarity;
    return (info.id >= 0) ? 0 : 2;                       // 0=认识 / 2=陌生人
}

// ── 注册 ──
int FaceRecognition::enroll(uint8_t* rgb, int w, int h, const char* name) {
    if (!_ready || !_detector || !_recognizer || !rgb || w <= 0 || h <= 0) return -1;

    std::list<dl::detect::result_t>& results = _detector->infer<uint8_t>(rgb, {h, w, 3});
    // 调试: 打印检测结果 (与 process 一致)
    if (!results.empty()) {
        float bestScore = results.begin()->score;
        for (auto it = results.begin(); it != results.end(); ++it)
            if (it->score > bestScore) bestScore = it->score;
        Serial.printf("[FACE-DBG] enroll detect: %d results, best score=%.3f\n",
                      (int)results.size(), bestScore);
    } else {
        Serial.println("[FACE-DBG] enroll detect: 0 results");
    }
    if (results.empty()) return -2;                      // 没检测到脸

    auto best = results.begin();
    for (auto it = results.begin(); it != results.end(); ++it) {
        if (it->score > best->score) best = it;
    }

    // 质量门槛 (防止糊脸/侧脸/太远注册出垃圾特征)
    int bx = best->box[0], by = best->box[1];
    int bw = best->box[2] - best->box[0];
    int bh = best->box[3] - best->box[1];
    if (best->score < FACE_ENROLL_MIN_SCORE) {
        Serial.printf("[FACE] enroll rejected: score %.3f < %.2f\n",
                      best->score, FACE_ENROLL_MIN_SCORE);
        return -3;
    }
    if (bw < FACE_ENROLL_MIN_BOX || bh < FACE_ENROLL_MIN_BOX) {
        Serial.printf("[FACE] enroll rejected: box %dx%d < %dpx (face too small)\n",
                      bw, bh, FACE_ENROLL_MIN_BOX);
        return -3;
    }
    Serial.printf("[FACE] enroll candidate: score=%.3f box=(%d,%d,%dx%d)\n",
                  best->score, bx, by, bw, bh);

    // 关键点: MSR01 不输出关键点, 从框估算 (align_face 需要 5 点=10 ints)
    std::vector<int> landmarks = best->keypoint;
    if (landmarks.size() < 10) {
        Serial.printf("[FACE] enroll: keypoint too small (%d), est from box\n", (int)landmarks.size());
        landmarks = landmarksFromBox(best->box);
    }
    Serial.printf("[FACE] landmarks size=%d, values=[%d,%d,%d,%d,%d,...]\n",
                  (int)landmarks.size(),
                  (landmarks.size()>0)?landmarks[0]:-1,
                  (landmarks.size()>1)?landmarks[1]:-1,
                  (landmarks.size()>2)?landmarks[2]:-1,
                  (landmarks.size()>3)?landmarks[3]:-1,
                  (landmarks.size()>4)?landmarks[4]:-1);
    dl::Tensor<uint8_t> in;
    in.set_element(rgb, false).set_shape({h, w, 3});

    std::string nm = (name && name[0]) ? name : "";
    int id = _recognizer->enroll_id(in, landmarks, nm, true);   // true=写 flash
    return id;
}

// ── 管理 ──
void FaceRecognition::setThreshold(float t) {
    if (_recognizer) _recognizer->set_thresh(t);
}

float FaceRecognition::getThreshold() const {
    return (_recognizer) ? _recognizer->get_thresh() : 0.55f;
}

void FaceRecognition::clearAll(bool updateFlash) {
    if (!_recognizer) return;
    int before = _recognizer->get_enrolled_id_num();
    _recognizer->clear_id(updateFlash);
    Serial.printf("[FACE] clear: %d -> 0 ids%s\n", before,
                  updateFlash ? " (flash synced)" : "");
}

void FaceRecognition::deleteLast(bool updateFlash) {
    if (!_recognizer) return;
    int remain = _recognizer->delete_id(updateFlash);
    Serial.printf("[FACE] delete last: %d id(s) remain%s\n",
                  remain, updateFlash ? " (flash synced)" : "");
}

void FaceRecognition::printIds() {
    if (!_recognizer) return;
    Serial.printf("[FACE] threshold=%.2f, enrolled=%d\n",
                  _recognizer->get_thresh(), _recognizer->get_enrolled_id_num());
    std::vector<face_info_t> ids = _recognizer->get_enrolled_ids();
    for (auto& f : ids) {
        Serial.printf("  id=%d name=\"%s\"\n", f.id, f.name.c_str());
    }
}
