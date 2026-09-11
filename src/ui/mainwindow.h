#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    void initUi();        // 初始化界面
    void initMenuBar();   // 初始化菜单栏
    void initToolBar();   // 初始化工具栏
    void initStatusBar(); // 初始化状态栏
};

#endif // MAINWINDOW_H