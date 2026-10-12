#include "menu.h"

void ListModel::reset(int count, int visibleRows, int selected) {
  count_ = count < 0 ? 0 : count;
  rows_ = visibleRows < 1 ? 1 : visibleRows;
  first_ = 0;
  select(selected);
}

void ListModel::select(int index) {
  if (count_ == 0) {
    selected_ = first_ = 0;
    return;
  }
  selected_ = index < 0 ? 0 : (index >= count_ ? count_ - 1 : index);
  follow();
}

void ListModel::next() {
  if (count_) select(selected_ + 1 < count_ ? selected_ + 1 : 0);
}

void ListModel::prev() {
  if (count_) select(selected_ > 0 ? selected_ - 1 : count_ - 1);
}

void ListModel::follow() {
  if (selected_ < first_) first_ = selected_;
  if (selected_ >= first_ + rows_) first_ = selected_ - rows_ + 1;
  const int maxFirst = count_ > rows_ ? count_ - rows_ : 0;
  if (first_ > maxFirst) first_ = maxFirst;
  if (first_ < 0) first_ = 0;
}
