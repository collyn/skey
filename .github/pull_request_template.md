## Tóm tắt

<!-- Mô tả ngắn: PR này làm gì, sửa vấn đề gì. -->

## Kiểm tra

<!-- Đánh dấu [x] những mục đã làm. -->

- [ ] Build OK: `cmake -B build -S . && cmake --build build -j$(nproc)`
- [ ] Settings GUI test (đổi cấu hình → Áp dụng → restart fcitx5)
- [ ] Gõ thử Telex / VNI (Surrounding Text + Uinput nếu động tới server/engine)

## Packaging

<!-- Chỉ cần khi PR đổi file trong data/, debian/, packaging/, build.sh hoặc .github/workflows/. -->

- [ ] `bash -n` các script sửa (build.sh, scripts/*)
- [ ] CI dev: push nhánh `dev` → prerelease `vX.Y.Z-dev.N` chạy xanh (deb/rpm/arch)
- [ ] Path cài đặt khớp `%files`/dpkg (udev, sysusers, polkit, unit, profile.d)

## Phiên bản / Kênh

<!-- Nhớ: nhánh dev luôn bump CMakeLists ≥ main; bản stable ra từ tag vX.Y.Z trên main. -->

- [ ] `project(fcitx5-skey VERSION ...)` trong CMakeLists.txt đã bump (nếu cần)
