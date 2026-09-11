#include "mainwindow.h"

#include <QStatusBar>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    resize(1200, 800);
    setWindowTitle("Markdown 编辑器");
    initUi();
    initMenuBar();
    initToolBar();
    initStatusBar();
}

MainWindow::~MainWindow() = default;

void MainWindow::initUi()
{
    // 后续填充中心控件
}

void MainWindow::initMenuBar()
{
    // 后续添加菜单
}

void MainWindow::initToolBar()
{
    // 后续添加工具栏
}

void MainWindow::initStatusBar()
{
    statusBar()->showMessage("就绪");
}