#include "homewindow.h"
#include <QApplication>
#include "easylogging++.h"

INITIALIZE_EASYLOGGINGPP    // 初始化宏，有且只能使用一次

#undef main
int main(int argc, char *argv[])
{

//    el::Loggers::reconfigureAllLoggers(el::ConfigurationType::Format, "%datetime %level %func(L%line) %msg");

    el::Configurations conf;
    conf.setToDefault();
    conf.setGlobally(el::ConfigurationType::Format, "[%datetime | %level] %func(L%line) %msg");
    conf.setGlobally(el::ConfigurationType::Filename, "log_%datetime{%Y%M%d}.log");
    conf.setGlobally(el::ConfigurationType::Enabled, "true");
    conf.setGlobally(el::ConfigurationType::ToFile, "true");
    el::Loggers::reconfigureAllLoggers(conf);
    el::Loggers::reconfigureAllLoggers(el::ConfigurationType::ToStandardOutput, "true"); // 也输出一份到终端

//    LOG(VERBOSE) << "logger test"; //该级别只能用宏VLOG而不能用宏 LOG(VERBOSE)
    LOG(TRACE) << " logger";
//    LOG(DEBUG) << "logger test";
    LOG(INFO) << "logger test";
    LOG(WARNING) << "logger test";
    LOG(ERROR) << "logger test";
    QApplication a(argc, argv);
    HomeWindow w;
    w.show();

    return a.exec();
}
