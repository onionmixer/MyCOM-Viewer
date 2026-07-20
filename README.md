# MYCOM Viewer

Release 0.7.0

MYCOM CD-ROM ISO에서 자료를 복원해 읽을 수 있게 만드는 두 개의 프로그램입니다.

```text
MYCOM.ISO (읽기 전용)
  → mycom-archive-build
  → MYCOM archive 디렉터리 (정규화 원본 + 변환 콘텐츠 + manifest)
  → mycom-viewer
```

`mycom-archive-build`는 내장 ISO9660 reader로 ISO를 직접 읽어 내부의 MVB,
DBF, BMP, WAV, AVI 파일을 추출·정규화·변환합니다.

## 설치 후 가장 먼저 할 일

배포 패키지에는 저작권·용량 문제로 MYCOM ISO와 변환 archive를 기본 포함하지
않습니다. 처음 사용할 때는 다음 중 하나를 준비해야 합니다.

- 이미 만들어 둔 `mycom-archive` 디렉터리: `manifest.json`을 포함해야 합니다.
- 원본 `MYCOM.ISO`: archive builder로 한 번 변환합니다.

viewer의 **File → ISO unpack...** 기능은 함께 설치된 archive builder와 내장
ISO9660 reader를 먼저 점검한 뒤 ISO를 변환하므로, 터미널을 열지 않고도 archive를
만들 수 있습니다.

## viewer에서 ISO unpack

1. **File → ISO unpack...**을 선택합니다.
2. MYCOM ISO 파일을 선택한 뒤, 비어 있는 archive 출력 폴더를 선택합니다.
3. viewer가 함께 배포된 `mycom-archive-build`와 내장 ISO9660 reader를 확인합니다.
4. 변환 로그 창에서 진행 상태를 확인합니다. 완료되면 새 archive가 viewer에 자동으로
   열립니다.

기존 archive가 있는 폴더를 고르면 명시적인 확인 후에만 재생성합니다. 취소된 작업의
부분 출력은 자동 삭제하지 않으므로, 필요할 경우 사용자가 해당 폴더를 검토·삭제해야
합니다. 마지막 ISO·출력 폴더는 사용자별 `mycom-viewer.ini`에만 기록됩니다.

## archive 만들기

ISO 파일은 변경하지 않으며, 출력 대상은 새 디렉터리 또는 빈 디렉터리여야
합니다.

```text
mycom-archive-build MYCOM.ISO mycom-archive
```

Windows에서는 보통 다음과 같이 실행합니다.

```bat
mycom-archive-build.exe C:\path\to\MYCOM.ISO C:\path\to\mycom-archive
```

내장 ISO9660 reader를 터미널에서 미리 점검하려면 다음을 실행합니다.

```text
mycom-archive-build --check-tools
```

기존에 정상 생성된 archive를 다시 만들려면 보호된 `--rebuild` 옵션을 사용합니다.

```text
mycom-archive-build --rebuild MYCOM.ISO mycom-archive
```

## archive 열기

viewer에서 **File → Open converted archive...**를 선택하고 `manifest.json`이 있는
archive 폴더를 지정합니다. 명령행에서도 열 수 있습니다.

```text
mycom-viewer mycom-archive
```

viewer는 기사·월별 목록·복원 텍스트를 탐색하고, 대소문자를 구분하지 않는 검색,
콘텐츠 안의 `Ctrl+F` 검색과 강조 표시, 북마크, 본문 글꼴/확대·축소, WAV/AVI
재생을 제공합니다. 북마크와 콘텐츠 글꼴은 archive가 아니라 사용자별
`mycom-viewer.ini`에 저장됩니다.

## 설치 패키지 사용법

### Windows

`MYCOM-Viewer-...-win64.exe` NSIS 설치 프로그램을 실행합니다. Qt 런타임과
viewer, archive builder가 함께 설치됩니다. 설치 마침 화면의 **README 열기**를
선택하면 이 안내를 바로 읽을 수 있으며, 시작 메뉴에도 README 바로가기가
있습니다.

ISO 변환은 **File → ISO unpack...**에서 수행할 수 있으며, 별도 추출 프로그램
설치가 필요하지 않습니다. 제거는 Windows **설치된 앱** 또는 MYCOM Viewer 시작
메뉴의 제거 항목을 사용합니다. 사용자가 별도로 만든 archive는 제거하지
않습니다.

### Ubuntu

다운로드한 DEB 파일을 설치합니다.

```bash
sudo apt install ./mycom-viewer_*.deb
```

응용 프로그램 메뉴에서 **MYCOM Viewer**를 실행하거나 터미널에서
`mycom-viewer`를 실행합니다. 문서는
`/usr/share/doc/mycom-viewer/README.txt`에 설치됩니다. ISO 변환은 추가 추출
도구 없이 GUI의 **File → ISO unpack...**에서 수행합니다. 제거는 다음과 같습니다.

```bash
sudo apt remove mycom-viewer
```

### macOS

`MYCOM-Viewer-...pkg`를 열어 Installer 절차를 따릅니다. Installer가 설치 전에
README를 표시하며, 설치 후에는 **mycom-viewer.app**의 Resources에도 같은
README가 포함됩니다. viewer는 Applications 폴더에서 실행합니다.

ISO 변환은 GUI의 **File → ISO unpack...**에서 수행할 수 있습니다. 이 기능은
`/usr/local/bin/mycom-archive-build`의 내장 ISO9660 reader를 사용합니다. package가
서명·공증되지 않은 개발 빌드라면 macOS 보안
설정에서 명시적으로 열기 승인이 필요할 수 있습니다.

## archive 형식

변환 결과는 다른 OS에서도 그대로 옮겨 열 수 있는 `mycom-archive/v1` 디렉터리
형식입니다.

```text
mycom-archive/
  manifest.json                 # 형식·ISO SHA-256·검증 정보
  normalized/
    mvb/                         # 정규화된 MVB 원본
    dbf/MYDBF01.DBF
    assets/{bmp,wav,myavi}/
  content/                       # viewer가 읽는 JSON 콘텐츠
```

`manifest.json`은 ISO SHA-256, 정규화 파일별 SHA-256·크기, MVB/asset 수,
DBF 레코드 수와 변환 결과를 기록합니다. viewer는 이 manifest를 검증한 뒤에만
archive를 엽니다.

## 개발·검증

CMake 3.16 이상, Qt 5.12 이상(Core, Widgets, Multimedia,
MultimediaWidgets), C++17 컴파일러가 필요합니다. ISO 통합 검증은 내장
ISO9660 reader를 사용합니다.

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

전체 ISO 통합 검증은 `MYCOM_ISO`를 지정해 실행합니다.

```bash
MYCOM_ISO=/absolute/path/to/MYCOM.ISO ctest --test-dir build -L iso --output-on-failure
```

`scripts/dev/run_mycom_viewer.sh`는 Linux/macOS 개발용 편의 스크립트이며 배포
패키지에는 포함되지 않습니다.

## 릴리즈 정보

현재 릴리즈는 **0.7.0**입니다. Windows x64 NSIS installer, Ubuntu amd64 DEB,
macOS x86_64 PKG를 제공합니다. Windows 32비트 패키지는 제공하지 않습니다.
변경 내역은 [CHANGELOG.md](CHANGELOG.md)에서 확인할 수 있습니다.
