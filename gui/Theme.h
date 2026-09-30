#pragma once
#include <QString>

class QLabel;
class QWidget;

namespace vsum {

// 应用全局样式表 (浅色 "工作台" 主题: 墨色顶栏 + 青绿色主色)。
// 颜色与字号全部集中在 Theme.cpp, 业务代码只通过 objectName 引用语义角色。
QString appStyleSheet();

// objectName / 动态属性变更后需要手动重新套用样式 (QSS 只在 polish 时解析选择器)。
void restyle(QWidget* w);

// 给 QLabel 套语义角色: "hintLabel" | "okLabel" | "errorLabel" | "warnLabel"
// | "stageLabel" | "resultLabel" | "sectionHint"。会立即重新应用样式。
void setLabelRole(QLabel* label, const char* role);

}  // namespace vsum
