MYCOM Viewer
============

Release 0.7.1

MYCOM CD-ROM ISO에서 자료를 복원해 읽을 수 있게 만드는 프로그램입니다.

  MYCOM.ISO (읽기 전용)
    -> mycom-archive-build
    -> mycom-archive (정규화 원본 + 변환 콘텐츠 + manifest)
    -> mycom-viewer

중요
----

* 내장 ISO9660 reader로 ISO를 직접 분해·변환합니다.
* 배포 패키지에는 ISO와 변환 archive가 기본 포함되지 않습니다.
* File -> ISO unpack...은 함께 배포된 archive builder와 내장 ISO9660 reader를 점검한 뒤 ISO를
  변환하고, 결과 archive를 자동으로 엽니다.
* MYCOM ISO와 여기에서 생성하는 원본 텍스트·이미지·미디어는 MIT 대상이 아닙니다.
  적법하게 보유한 원본에서만 archive를 생성·사용하십시오.

처음 사용하기
-------------

1. 이미 만든 mycom-archive 폴더(manifest.json 포함)가 있으면 viewer에서
   File -> Open converted archive...를 선택해 해당 폴더를 엽니다.
2. ISO만 있다면 File -> ISO unpack...을 선택하고 ISO와 비어 있는 출력 폴더를
   지정합니다. 외부 추출 도구는 필요하지 않습니다. 기존 archive는 명시적으로 확인한 경우에만 재생성하며, 취소한 작업의 부분 출력은
   자동 삭제하지 않습니다.
3. 터미널 작업이 필요하면 archive를 직접 만듭니다.

  mycom-archive-build MYCOM.ISO mycom-archive

Windows 예시:

  mycom-archive-build.exe C:\path\to\MYCOM.ISO C:\path\to\mycom-archive

내장 ISO9660 reader 점검만 실행하려면 다음을 사용합니다.

  mycom-archive-build --check-tools

기존 archive를 다시 만들려면 다음을 사용합니다. 정상 archive만 삭제할 수
있도록 보호되어 있습니다.

  mycom-archive-build --rebuild MYCOM.ISO mycom-archive

Archive 열기
-----------

  mycom-viewer mycom-archive

viewer는 기사·월별 목록·복원 텍스트 탐색, 대소문자 구분 없는 검색, 콘텐츠 안의
Ctrl+F 검색과 강조 표시, 북마크, 본문 글꼴/확대·축소, WAV/AVI 재생을 지원합니다.
북마크와 콘텐츠 글꼴은 사용자별 mycom-viewer.ini에 저장됩니다.

플랫폼별 설치
---------------

Windows
  NSIS 설치 프로그램을 실행합니다. 설치 마침 화면에는 MYCOM Viewer 실행과
  README 열기가 독립된 기본 선택 항목으로 표시됩니다. 시작 메뉴에도 viewer와
  README 바로가기가 있습니다.
  ISO 변환은 File -> ISO unpack...에서 수행할 수 있으며 별도 추출 프로그램 설치가
  필요하지 않습니다. Windows 설치된 앱 또는 시작 메뉴의 제거 항목으로 제거합니다.
  사용자가 만든 archive는 제거하지 않습니다.
  설치 폴더의 share/doc/mycom-viewer에 MIT, Qt LGPLv3/GPLv3 전문 및 third-party
  notices가 함께 설치됩니다.

Ubuntu
  sudo apt install ./mycom-viewer_*.deb

  응용 프로그램 메뉴의 MYCOM Viewer 또는 mycom-viewer 명령으로 실행합니다.
  이 문서는 /usr/share/doc/mycom-viewer/README.txt에 설치됩니다. ISO 변환에는
  추가 추출 도구가 필요하지 않습니다.
  같은 문서 폴더에 LICENSE, THIRD_PARTY_NOTICES.md 및 Qt 라이선스 전문이 있습니다.

  sudo apt remove mycom-viewer

macOS
  MYCOM-Viewer-...pkg를 열어 Installer 절차를 따릅니다. Installer가 설치 전
  README를 표시하고, 설치 후에는 mycom-viewer.app의 Resources에도 이 문서가
  들어 있습니다. viewer는 Applications 폴더에서 실행합니다.

  GUI의 File -> ISO unpack...은 /usr/local/bin/mycom-archive-build의 내장
  ISO9660 reader를 사용합니다.
  /usr/local/share/doc/mycom-viewer에 라이선스 문서가 설치되며, 앱 번들 Resources에도
  LICENSE와 THIRD_PARTY_NOTICES.md가 포함됩니다.
  서명·공증되지 않은 개발 패키지는 macOS 보안 설정에서 열기를 승인해야 할 수
  있습니다.

Archive 형식
------------

  mycom-archive/
    manifest.json                 형식, ISO SHA-256, 검증 정보
    normalized/mvb/                정규화된 MVB 원본
    normalized/dbf/MYDBF01.DBF
    normalized/assets/{bmp,wav,myavi}/
    content/                       viewer가 읽는 JSON 콘텐츠

manifest.json은 ISO SHA-256, 정규화 파일별 SHA-256·크기, MVB/asset 수,
DBF 레코드 수와 변환 결과를 기록합니다.

릴리즈 정보
-----------

현재 릴리즈는 0.7.1입니다. Windows x64 NSIS installer, Ubuntu amd64 DEB,
macOS x86_64 PKG를 제공합니다. Windows 32비트 패키지는 제공하지 않습니다.
자세한 변경 내역은 CHANGELOG.md를 확인하십시오.

라이선스
--------

MYCOM Viewer의 자체 소스와 자체 제작 리소스는 LICENSE의 MIT License로
제공됩니다. Windows/macOS 패키지의 Qt 런타임은 별도 LGPLv3 조건이며,
THIRD_PARTY_NOTICES.md와 설치된 LGPLv3/GPLv3 전문을 함께 확인하십시오.
