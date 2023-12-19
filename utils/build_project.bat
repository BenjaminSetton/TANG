@echo off

:: Generate a Visual Studio solution
:: Visual Studio 2019: "vs2019"
:: Visual Studio 2022: "vs2022"
set GENERATOR="vs2022"
set BUILD_DIR="build"

echo Building project for %GENERATOR%...

pushd ..
call "vendor/premake/premake5.exe" %GENERATOR%
popd

echo Finished building project in %BUILD_DIR% folder!

pause