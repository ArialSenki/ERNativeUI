@echo off
rem Builds the talk-menu static library linked into ERNativeUI.dll ("build.cmd
rem test" runs its tests). Rust: cargo from PATH, or a portable install with
rem rustup\ and cargo\ in NT_RUST_HOME; MSVC 2022. CMake calls this before
rem linking ERNativeUI.dll. Set NT_CARGO_OFFLINE=1 to build only from crates
rem already downloaded.
setlocal
if not "%NT_RUST_HOME%"=="" (
  set "RUSTUP_HOME=%NT_RUST_HOME%\rustup"
  set "CARGO_HOME=%NT_RUST_HOME%\cargo"
  set "PATH=%NT_RUST_HOME%\cargo\bin;%PATH%"
)
set "OFFLINE="
if "%NT_CARGO_OFFLINE%"=="1" set "OFFLINE=--offline"
if "%ERUI_TALK_TARGET_DIR%"=="" set "ERUI_TALK_TARGET_DIR=%LOCALAPPDATA%\ERNativeUI-talk-build"
rem Source paths that panic messages embed are shortened to /work (the folder
rem that holds NativeTweaks and MultipleSpiritAshes) and /cargo, so the DLL
rem carries no local folder names.
for %%I in ("%~dp0..\..\..") do set "ERUI_TALK_WORK=%%~fI"
set "ERUI_TALK_CARGO=%CARGO_HOME%"
if "%ERUI_TALK_CARGO%"=="" set "ERUI_TALK_CARGO=%USERPROFILE%\.cargo"
set REMAP=--config "build.rustflags=['--remap-path-prefix=%ERUI_TALK_WORK%=/work','--remap-path-prefix=%ERUI_TALK_CARGO%=/cargo']"
cd /d "%~dp0"
if /i "%1"=="test" (
  cargo test --release %OFFLINE% %REMAP% --target x86_64-pc-windows-msvc --target-dir "%ERUI_TALK_TARGET_DIR%" --lib
  exit /b %errorlevel%
)
cargo build --release %OFFLINE% %REMAP% --target x86_64-pc-windows-msvc --target-dir "%ERUI_TALK_TARGET_DIR%"
exit /b %errorlevel%
