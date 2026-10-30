#pragma once
#include <QIcon>
#include <QPalette>

enum class ConnectionIconState { Idle, Running, Working, Unavailable };
QIcon applicationIcon(const QPalette& palette, ConnectionIconState state);
