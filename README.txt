MYCOM Archive Builder / Qt5 Viewer
==================================

1. 개요
-------

이 프로젝트는 MYCOM CD의 원본 ISO를 읽어 정규화된 아카이브를 만들고,
Qt5 프로그램으로 아카이브를 열람합니다.

원본 ISO는 수정하지 않습니다.

  MYCOM.ISO
      -> mycom-archive-build
      -> 아카이브 디렉터리
      -> mycom-viewer

실행 파일은 두 개입니다.

  mycom-archive-build : ISO 추출, 정규화, MVB/DBF 분석 및 변환
  mycom-viewer        : manifest 기반 아카이브 Qt5 열람기

7z는 ISO를 읽기 위해 빌더가 호출하는 외부 도구입니다. 별도 프로젝트나
수동 추출 단계는 필요하지 않습니다.


2. 요구 사항
------------

  - CMake 3.16 이상
  - C++17 컴파일러
  - Qt 5.12 이상
      Core, Widgets, Multimedia, MultimediaWidgets
  - 7z 실행 파일


3. 빌드
--------

  cmake -S . -B build
  cmake --build build


4. 아카이브 생성
-----------------

  ./build/mycom-archive-build MYCOM.ISO mycom_archive

7z가 PATH에 없으면 다음과 같이 지정합니다.

  ./build/mycom-archive-build --seven-zip /path/to/7z MYCOM.ISO mycom_archive

출력 대상 디렉터리는 새 디렉터리이거나 비어 있어야 합니다.
이미 생성된 유효한 MYCOM 아카이브를 갱신하려면 다음을 사용합니다.

  ./build/mycom-archive-build --rebuild MYCOM.ISO mycom_archive

--rebuild는 manifest.json이 있고 형식이 확인된 MYCOM 아카이브만 삭제합니다.
임의의 비어 있지 않은 디렉터리는 삭제하지 않습니다.

선택 옵션:

  --topic-pages  기사 단위의 정적 HTML을 content/topics에 생성
  --review-html  원시 복원 텍스트 및 탐색 보고서 HTML 생성
  --min-bytes N  진단용 최소 텍스트 연속 길이 지정

Qt5 뷰어에 필요한 것은 JSON이므로, 정적 HTML은 기본으로 생성하지 않습니다.


5. 생성되는 아카이브 구조
---------------------------

  mycom_archive/
    manifest.json
    normalized/
      mvb/                       정규화된 MVB 파일
      dbf/MYDBF01.DBF            원본 DBF 카탈로그
      assets/
        bmp/
        wav/
        myavi/
    content/
      HEADA.json 등              Qt5 뷰어용 변환 데이터

manifest.json은 다음 정보를 기록합니다.

  - ISO 파일명, 크기, 수정 시각, ISO SHA-256
  - 정규화 MVB/DBF/BMP/WAV/AVI 파일 목록
  - 각 정규화 파일의 canonical path, byte 크기, SHA-256
  - MVB 수, 자산 수, DBF 기사 수, 변환 책 수
  - content 형식과 선택 생성된 정적 HTML 종류


6. Qt5 뷰어 실행
-----------------

  ./build/mycom-viewer mycom_archive

뷰어는 manifest.json을 먼저 검증하고 content 및 normalized/assets 경로를
확인한 뒤 아카이브를 엽니다.

주요 기능:

  - 월별 목록, 기사, 복원 텍스트 열람
  - 전체 아카이브 검색 및 오른쪽 내용 영역 내 Ctrl+F 검색
  - 검색어 강조, 대소문자 비구분 검색
  - 내용 글꼴 선택 및 10% 단위 확대/축소
  - 북마크
  - WAV/AVI 미디어 재생

글꼴과 북마크 설정은 사용자 설정의 mycom-viewer.ini에 저장됩니다.


7. 검증
--------

일반 단위 테스트:

  ctest --test-dir build --output-on-failure

실제 ISO 전체 통합 테스트:

  MYCOM_ISO=/absolute/path/to/MYCOM.ISO \
    ctest --test-dir build -L iso --output-on-failure

MYCOM_ISO가 설정되면 ISO 전체 빌드 후 다음을 검증합니다.

  - manifest 형식 및 schemaVersion
  - ISO SHA-256
  - MVB 17개
  - DBF 기사 2,010건
  - 변환 책 17개
  - 정규화 파일 7,688개의 SHA-256 목록

MYCOM_ISO가 없으면 ISO 통합 테스트는 건너뛰며, 일반 단위 테스트는
원본 ISO 없이 실행할 수 있습니다.


8. 원본 데이터 관련 주의
-------------------------

HEADA의 DBF 기사 카탈로그에는 원본 MVB 토픽/본문 근거가 없는 5건이 있습니다.
이 항목들은 다른 달의 유사 기사로 임의 연결하지 않고 catalog-only로 유지합니다.

  92120690
  94023200
  9410080_
  95113560
  95124060
