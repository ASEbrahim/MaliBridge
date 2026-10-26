Arm HWCPipe (https://github.com/ARM-software/HWCPipe, MIT, Copyright (c) 2022-2026 Arm Limited).
No HWCPipe file is copied here. server/server_hwcnt.c re-implements the kbase kinstr_prfcnt
ioctl layouts from its backend/device sources and takes counter indices for Mali-G720, Mali-G725
(Immortalis-G925) and Mali G1 from its specification/database; that file carries the MIT notice
for those parts, and LICENSE.md here is HWCPipe's licence as published (read at commit 378c7b5).
