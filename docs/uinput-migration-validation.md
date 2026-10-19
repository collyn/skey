# Kết quả chuyển Uinput vào fcitx5

Triển khai trên HEAD `4b1efe9`, ngày 2026-10-19. Chưa cài vào hệ thống,
chưa restart fcitx5 của phiên làm việc, chưa commit.

## Phạm vi đã triển khai

- Một `UinputInjector` thuộc engine, worker tạo thiết bị và phát Backspace/Escape.
  Không dùng socket/server; không chạy timing I/O trên luồng xử lý phím.
- Giữ thuật toán tiếng Việt, cấu hình mode, Native XTest/libei, số deletion,
  sync-anchor, tính pacing, EWMA, settle và replay hiện có.
- Giữ warmup tạo thiết bị 1 giây trên worker, bắt đầu từ lúc addon load.
  Giá trị pacing 0 vẫn dùng mặc định 1 ms như server cũ.
- Queue tối đa 8 batch chờ, không ghi đè; mỗi request có ticket kết quả,
  mỗi IC có owner riêng để hủy đúng batch. Request ngoài giới hạn 1–64
  bị từ chối toàn bộ, tránh xóa thiếu rồi commit replacement.
- Kiểm tra lỗi open/configure/write, ghi thiếu, EINTR/EAGAIN, retry 1 giây,
  hủy và shutdown. Không tự phát lại batch lỗi. Engine quan sát ticket
  trên main loop; lỗi hủy pending commit và dùng lại grace loopback 400 ms.
- Giữ guard reset/deactivate giả của browser. Reset thật, đổi cell, IC bị
  hủy và focus timeout hủy công việc Uinput của đúng owner.
- Rule `70-skey-uinput.rules` dùng uaccess trước `73-seat-late.rules`.
  modules-load nạp uinput khi boot; package hook nạp/trigger khi cài.
- Debian, build.sh, RPM, Arch, setup và GUI bỏ đường start/restart server.
  Helper migration stop/disable instance cũ, dọn symlink wants/requires còn
  treo, unit/drop-in trong /etc, /run, /usr/local, /usr/lib và /lib; dọn binary
  cũ từ source, rule udev/polkit, sysusers và socket runtime đúng tên SKey.
  Local edits được lưu ngoài thư mục hoạt động tại
  `/var/lib/fcitx5-skey/legacy-uinput/`, không ghi đè bản sao cũ.
  Chỉ xóa ACL mang tên sysuser cũ, giữ tài khoản để tránh tái sử dụng uid.
  Lỗi stop hoặc process server cũ còn sống làm migration báo lỗi, không
  tiếp tục restart fcitx5. Offline/chroot không yêu cầu systemd đang chạy.
  Cài source trực tiếp vào /usr hoặc /usr/local tự gọi helper; DESTDIR và
  Nix/store/custom-prefix không chạy migration trên host.
- NixOS dùng hardware.uinput và services.udev.packages; option users cũ
  vẫn được chấp nhận, phát cảnh báo deprecation và không tạo service.

## Kiểm tra đã chạy

| Kiểm tra | Kết quả |
| --- | --- |
| Baseline CTest ngoài sandbox, sau sửa fixture Office | 11/11 pass |
| Build CMake đầy đủ | Pass |
| CTest sau tích hợp | 13/13 pass |
| Injector độc lập qua Clang, `-Wall -Wextra -Wpedantic -Werror` | Build và chạy pass |
| Toàn bộ project qua Clang 21, `-Wall -Wextra -Werror` | Build đầy đủ pass; CTest 13/13 pass |
| clang-include-cleaner 21, `--strict` | 31 file, 0 suggestions, 0 tool errors |
| Unit injector | Thứ tự key/SYN, Escape, pacing, queue, owner cancellation, short write, EINTR/EAGAIN, lỗi vĩnh viễn, zero write, retry, shutdown/warmup pass |
| Engine | Thiếu readiness, raw pass-through khi reject/drain, Surrounding fallback lỗi, partial failure, loopback muộn; regression tiếng Việt/Office/omnibox/WebKit/focus pass |
| Migration trong root tạm, mock lệnh hệ thống | Upgrade lặp lại, backup không ghi đè, /etc/source overrides, merged-usr, offline, không có loaded unit, stop lỗi, orphan process, ACL riêng và remove pass |
| `udevadm verify` | Rule hợp lệ |
| Bash/sh syntax, RPM spec expansion và syntax scriptlet | Pass |
| DESTDIR install | 78 file, có addon/rule/modules-load/helper, không server/unit/polkit/sysusers |
| `git diff --check` | Pass |

CTest cần chạy ngoài sandbox vì sandbox chặn Unix socket send và D-Bus bind.
Các test dùng I/O giả, không phát phím vào ứng dụng đang mở. Fixture Office
chỉ bổ sung drain timer replay sẵn có; expected text và số deletion giữ nguyên.
Logic Native, Vietnamese và config không thay đổi. GitNexus đánh dấu vùng
engine có nhiều caller; đã rà các đường send, focus và fallback cùng diff thực.

Log cục bộ: `/tmp/skey-uinput-baseline-unsandboxed.log`,
`/tmp/skey-uinput-audit-build.log`, `/tmp/skey-uinput-audit-tests.log`,
`/tmp/skey-uinput-audit-stage.log`. Lượt Clang đầy đủ:
`/tmp/skey-uinput-clang-build-final.log`, `/tmp/skey-uinput-clang-tests-final.log`,
`/tmp/skey-uinput-includes-final.log`. Bản cài staging: `/tmp/skey-uinput-audit-stage`.

## Các rủi ro phát hiện khi rà soát bổ sung

- Safety timer có thể commit khi worker vẫn phát một batch pacing dài.
  Đã tái hiện bằng regression thất bại trước sửa; cả safety timer (gồm
  auto-restore/omnibox) và settle timer nay đợi ticket rời trạng thái Queued.
  Native không có ticket nên giữ timing cũ. Test xác nhận pending text không
  bị lấy ra sớm và chỉ commit một lần sau khi worker Complete.
- Từ chối deletion sau khi consume phím có thể mất ký tự trigger. Đường suffix
  nay chỉ consume sau khi chấp nhận; trong grace hủy batch, phím mới đi qua raw.
- Cleanup trước đây bỏ sót rule /etc, source, drop-in và che lỗi stop.
  Đã hợp nhất helper cho install/upgrade/remove, mở rộng các path legacy và
  kiểm tra orphan theo executable thực trong /proc, không kill theo substring.
- Đã bỏ server/socket runtime cũ, đường GUI/setup restart service, hook trùng
  stop/disable và comment/include thừa. Thuật toán tiếng Việt và logic Native
  giữ nguyên; diff/check và regression xác minh các đường thông thường.
- Giữ Escape ở nhánh omnibox hiện tại. Clang cleanup chỉ bỏ `<cerrno>` không
  còn dùng sau khi bỏ socket, dọn comment/whitespace và đánh dấu public umbrella
  `<libportal/portal.h>` là include có chủ đích cho libportal; không xóa header
  này theo gợi ý tự động vì các declaration đến từ các component được nó include.

## Chưa xác minh trên môi trường thật

- X11/Wayland compositor nhận thiết bị, chọn IM rồi gõ ngay sau login/restart,
  chuyển IM/focus dưới tải và chấp nhận Backspace/commit ở renderer thật.
- Terminal, Chrome page/omnibox, Firefox, Electron, LibreOffice, Sheets/Docs:
  Telex/VNI chậm/burst và không commit vào focus mới.
- Fresh install, upgrade, reinstall, remove/purge trên Debian/Ubuntu, Arch,
  Fedora/openSUSE; ACL thực tế của active seat và lần gõ đầu tiên sau cài.
- NixOS parse/eval/build/switch: môi trường hiện tại không có Nix.

Quyền uaccess phụ thuộc logind/active local seat; phiên remote/headless không
được cấp quyền này tự động. Quyền mở /dev/uinput áp dụng cho các process cùng
user, không chỉ riêng addon. ACL bị thu hồi không đóng được fd đã mở; một tap
đã bắt đầu có thể hoàn tất key-release sau cancel. Grace loopback 400 ms vẫn
không phân biệt được Backspace vật lý và Backspace được inject; sau lỗi/hủy,
Backspace thật trong khoảng đó có thể bị nuốt. Worker Complete chỉ xác nhận
đã ghi vào kernel; renderer/app vẫn dùng anchor/settle/deadline hiện có. Đây là
các giới hạn cần kiểm tra khi đổi focus/seat thật, không thể chứng minh hết
bằng mock.

Không dùng kết quả unit/staging để kết luận desktop đã nghiệm thu. Không mở
`/dev/uinput` thật trong lượt triển khai này. Hướng dẫn kiểm thử và rollback
ở `scripts/TESTING.md`.
