# MYCOM Viewer release history

## 0.7.1

- 자체 소스와 자체 제작 리소스에 MIT License를 적용했습니다.
- Windows/macOS 패키지에 Qt LGPLv3/GPLv3 전문과 third-party notices를 설치하도록
  보강했습니다.
- MYCOM ISO와 변환된 원본 콘텐츠가 프로젝트 MIT 라이선스의 대상이 아님을
  README에 명시했습니다.
- Windows NSIS installer의 완료 화면에 MYCOM Viewer 실행과 README 열기를
  별도 선택 항목으로 제공하도록 보완했습니다.
- macOS 패키징 전 disposable stage/package 출력만 정리해 과거 설치 레이아웃의
  잔여물이 새 PKG에 섞이지 않도록 했습니다.

## 0.7.0

- MYCOM ISO를 직접 분해·정규화·변환하는 archive builder와 Qt5 viewer를
  하나의 배포 단위로 정리했습니다.
- archive builder에 외부 의존성 없는 ISO9660 reader를 내장했습니다. MYCOM ISO의
  MVB, DBF, BMP, WAV, AVI 추출에 별도 프로그램이 필요하지 않습니다.
- viewer의 **File → ISO unpack...**에서 ISO와 출력 폴더를 선택하면, 함께
  배포된 archive builder와 내장 ISO9660 reader를 사전 점검하고 비동기로 변환한 뒤
  결과를 자동으로 엽니다.
- archive builder의 `--check-tools`는 내장 ISO9660 reader의 사용 가능 상태를
  확인합니다.
- viewer는 Windows/Linux에서 함께 설치된 실행 파일 옆의 archive builder를,
  macOS에서는 `/usr/local/bin`의 archive builder를 찾아 실행합니다.
- Windows x64 NSIS installer를 제공합니다. 설치 완료 화면과 시작 메뉴에서
  README를 열 수 있습니다.
- Ubuntu amd64 DEB를 제공합니다. Qt5 런타임 의존성, desktop entry, 아이콘과
  문서를 포함합니다.
- macOS x86_64 PKG를 제공합니다. Installer의 설치 전 README와 앱 번들 내부
  README를 포함합니다.
- archive manifest의 Windows drive/UNC 경로 검증을 강화했습니다.
- Windows 32비트 패키지는 제공하지 않습니다.

## Archive compatibility

- 변환 archive 형식: `mycom-archive/v1`
- manifest schema: `1`
