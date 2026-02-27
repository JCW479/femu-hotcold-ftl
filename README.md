# 🔥❄️ femu-hotcold-ftl

> SSD Write Amplification 48% 감소 — FEMU Blackbox FTL에 Hot/Cold 데이터 분리 기법 적용

**Advanced System Programming Final Project** | Hanyang University, 2024  
*Team: 김나현, 지찬웅*

---

## 📊 Results

| Metric | Baseline FTL | Optimized FTL | Improvement |
|--------|:-----------:|:-------------:|:-----------:|
| **WAF** | 7.67 | **3.94** | **↓ 48.6%** |
| **GC Overhead** | 667% | **294%** | **↓ 55.9%** |

Workload: FIO Zipf 0.99, 1시간 (Hot: 4KB random write × 4 threads / Cold: 128KB sequential write × 1 thread)

---

## 📌 Overview

기존 Blackbox FTL은 Hot/Cold 데이터를 같은 블록에 혼재하여 저장하기 때문에, GC 시 불필요한 Valid page 복사가 발생해 WAF가 급격히 증가합니다.

본 프로젝트는 **접근 패턴 기반 Hot/Cold 분류 + Dual Line Pool + Pool별 GC 정책**을 도입하여 WAF를 개선합니다.

---

## 🏗️ Architecture

### 1. Hot/Cold Classification

접근 횟수만으로는 "초반 몰림형" 등 패턴 구분이 불가능합니다.  
**접근 횟수(양적 기준) + Update Interval(시간적 기준)** 2중 조건을 사용합니다.

> *MiDAS (FAST '24)* 의 Update Interval 기반 Hotness 판단 방식에서 아이디어를 얻었습니다.

```
Hot 승격 조건:
  access_cnt >= 3              (HOT_ACCESS_THRESHOLD)
  delta <= 64 pages            (HOT_INTERVAL_THRESHOLD_PAGES)
  short_int_cnt >= 2           (HOT_INTERVAL_CONFIRM_COUNT)

Cold 강등 조건:
  access_cnt < 3
  delta > 256 pages
```

각 LPN마다 아래 메타데이터를 추적합니다:

```c
lpn_state[lpn]           // LPN_STATE_HOT / LPN_STATE_COLD
lpn_access_cnt[lpn]      // 최근 window 내 접근 횟수
lpn_last_write_seq[lpn]  // 마지막 쓰기 시점 (host_writes 기준)
lpn_short_int_cnt[lpn]   // 짧은 interval 연속 발생 횟수
```

### 2. Dual Line Pool

```
Total 128 Lines
├── Hot Pool  (20%, 25 lines)  ── wp_hot
└── Cold Pool (80%, 103 lines) ── wp_cold
```

- Hot write → `wp_hot`으로 할당 / Cold write → `wp_cold`로 할당
- 초기 설계(30:70)는 Cold pool 고갈 문제로 실패 → 20:80으로 조정

**Pool 고갈 처리:**

| 상황 | 1순위 | 2순위 |
|------|-------|-------|
| Hot Pool 고갈 | Hot GC (invalid ≥ 12.5%) | Emergency GC |
| Cold Pool 고갈 | Cold GC (invalid ≥ 30%) | Hot→Cold Demotion |
| 둘 다 고갈 | Aggressive GC (구분 없이 강제 회수) | — |

### 3. Pool별 GC 정책

| Pool | Policy | Score |
|------|--------|-------|
| **Hot** | Greedy | `ipc` (invalid page count) |
| **Cold** | Cost-Benefit | `Age × Invalid_Ratio` |

- **Hot**: 빠른 Invalid 누적 → Greedy로 즉시 공간 회수
- **Cold**: 오래되고 Invalid 비율 높은 line 우선 선택 → 장기 WAF 최소화
- Emergency GC에도 최소 invalid 조건 적용 (≥ 25%) → 대량 Valid 복사 사고 방지

---

## 📁 Files

```
.
├── ftl.c          # FTL 전체 구현 (Hot/Cold 분류, Dual Pool, GC 정책)
├── ftl.h          # 자료구조, 매크로, 함수 프로토타입
└── README.md
```

FEMU 원본 대비 수정 파일: `hw/femu/bbssd/ftl.c`, `hw/femu/bbssd/ftl.h`

---

## 🚀 Getting Started

### Prerequisites

```bash
# Ubuntu 20.04 / 22.04 권장
sudo apt-get install -y libglib2.0-dev libfdt-dev libpixman-1-dev zlib1g-dev \
    libnfs-dev libiscsi-dev git python3 python3-pip ninja-build meson \
    libaio-dev fio
```

### 1. FEMU 클론

```bash
git clone https://github.com/MoatLab/FEMU.git
cd FEMU
```

### 2. 이 레포의 파일로 교체

```bash
# 이 레포 클론
git clone https://github.com/<your-id>/femu-hotcold-ftl.git

# 파일 교체
cp femu-hotcold-ftl/ftl.c FEMU/hw/femu/bbssd/ftl.c
cp femu-hotcold-ftl/ftl.h FEMU/hw/femu/bbssd/ftl.h
```

### 3. FEMU 빌드

```bash
cd FEMU
mkdir build && cd build

# FEMU 공식 빌드 스크립트 사용
../femu-compile.sh
```

### 4. FEMU 실행 (Blackbox 모드)

```bash
# FEMU 공식 스크립트 참고 (femu/scripts/run-blackbox.sh)
sudo x86_64-softmmu/qemu-system-x86_64 \
    -drive file=<your-disk-image>,if=none,id=mynvme \
    -device femu,devsz_mb=65536,femu_mode=1 \
    ...
```

> 상세 QEMU 실행 옵션은 [FEMU 공식 문서](https://github.com/MoatLab/FEMU) 참고

### 5. 워크로드 실행 (FIO)

```bash
# Hot job (4KB random write)
fio --name=hot --rw=randwrite --bs=4k --numjobs=4 \
    --filename=/dev/nvme0n1 --ioengine=libaio \
    --iodepth=32 --runtime=3600 --time_based &

# Cold job (128KB sequential write)
fio --name=cold --rw=write --bs=128k --numjobs=1 \
    --filename=/dev/nvme0n1 --ioengine=libaio \
    --iodepth=32 --runtime=3600 --time_based &
```

### 6. WAF 확인

WAF 통계는 약 1GB(16,384 pages) 쓰기마다 자동으로 출력됩니다:

```
[FEMU] FTL-Log: ========== WAF Statistics ==========
[FEMU] FTL-Log: Host Writes: 250576896 pages (≈ 3823 GiB)
[FEMU] FTL-Log: NAND Writes: 988721282 pages (≈ 15079 GiB)
[FEMU] FTL-Log: WAF: 3.9440
[FEMU] FTL-Log: GC Overhead: 294.40%
[FEMU] FTL-Log: Free Lines: 17 / 128 (13.3%) [hot=9, cold=8]
```

---

## ⚠️ Limitations & Future Work

**현재 한계점**
- Hot/Cold 이분법 → "Warm" 데이터 처리 불완전
- Threshold(3, 64, 256) 고정 → 워크로드마다 최적값 상이
- Pool 비율(20:80) 고정 → 워크로드마다 동적 조정 필요

**개선 방향**
- Multi-tier 분류 (Hot-Warm-Cold)
- ML 기반 adaptive threshold
- Dynamic pool resizing
- Per-LBA 통계 기반 예측 모델

---

## 📚 References

- Oh et al., *"MiDAS: Minimizing Write Amplification in Log-Structured Systems through Adaptive Group Number and Size Configuration"*, FAST '24
- [FEMU: A NAND SSD Emulator (MoatLab)](https://github.com/MoatLab/FEMU)

---

## 👥 Team

| 이름 | 학번 |
|------|------|
| 김나현 | 2024214159 |
| 지찬웅 | 2025164485 |

*Hanyang University — Advanced System Programming, 2025*
