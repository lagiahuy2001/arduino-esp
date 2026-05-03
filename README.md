# Arduino Firmware

Hai firmware **chạy song song** trên thiết bị thực tế (admin có thể cài 2 chip vật lý lên cùng 1 máy):

## `firestore/` — Firmware chính (mới)

- Generic: chỉ đọc N pin, gửi state lên **Firestore**.
- Mapping (pin → component → device) hoàn toàn ở backend.
- Mỗi chip có `CHIP_ID` hardcode, phải khớp chip đăng ký trong admin UI.
- Debounce 50ms, tự khôi phục `startTime` sau restart, buffer khi mất mạng.
- Mỗi event Firestore dùng **deterministic documentId** = `{pin}_{epoch}_{state}` →
  resend không nhân đôi (Firestore trả 409 → coi như success).
- Buffer lưu kèm `epoch_at_event` → resend giữ đúng thời điểm sự kiện thật.

**Trước khi flash:**
1. Sửa `CHIP_ID` (dòng đầu code) — duy nhất cho mỗi chip.
2. Sửa `mac[]` — duy nhất cho mỗi chip nếu chạy nhiều chip cùng LAN.
3. Sửa `PINS[]` và `NAMES[]` cho khớp ChipType. **Không dùng GPIO 34/35/36/39**
   trừ khi có pull-up rời 10 kΩ (input-only, không có pull-up nội).
4. Network (IP, gateway, DNS).
5. Firestore Rules:

```
rules_version = '2';
service cloud.firestore {
  match /databases/{database}/documents {
    match /chip_events/{chipId}/events/{event} {
      // Khuyến nghị: bật App Check, hoặc đẩy event qua backend.
      // Rule public bên dưới CHỈ phù hợp lab/demo.
      allow create: if true;
      allow read:   if false;
      allow update, delete: if false;
    }
  }
}
```

> ⚠️ Rule public + API key nhúng firmware = ai có firmware đều spam được.
> Production nên: (a) bật Firebase App Check, (b) hoặc gửi event qua backend
> với JWT/HMAC, backend mới ghi Firestore.

## `google-sheet/` — Firmware cũ (giữ song song)

- Code gốc dùng Google Apps Script gửi vào Excel theo `device_id`.
- Mỗi user tự config link Excel của mình bên User Site → Excel Viewer để xem data này.
- KHÔNG ảnh hưởng tới hệ chính (Firestore).
- Đã thêm debounce 50ms và auto "đóng/mở" chu kỳ khi đổi mode (MAY_PHAT ↔ CAN_CAU)
  để tránh uptime gán nhầm thiết bị.
