// Generates PNGs of the logo (used to build inkpdf.ico). Not part of the app build.
#include <QGuiApplication>
#include "../logo.h"
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    for (int s : {16, 24, 32, 48, 64, 128, 256}) makeLogo(s).save(QString("/tmp/logo_%1.png").arg(s));
    return 0;
}
