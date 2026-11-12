@echo off
rem Usage: deploy.bat path\to\folder\containing\InkPDF.exe   (run from a Qt command prompt)
set D=%~1
windeployqt6 --release --no-translations --no-opengl-sw --no-system-d3d-compiler --no-compiler-runtime "%D%\InkPDF.exe"
rem InkPDF does not use these plugins; removing them keeps the folder small
for %%P in (imageformats iconengines tls networkinformation generic) do if exist "%D%\%%P" rmdir /s /q "%D%\%%P"
echo Done. Test the app once; if something is missing, re-run windeployqt without the rmdir step.
