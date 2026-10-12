// Selection and scrolling for a vertical list with a fixed number of visible rows.
#pragma once

class ListModel {
 public:
  void reset(int count, int visibleRows, int selected = 0);
  void next();  // wraps to the top
  void prev();  // wraps to the bottom
  void select(int index);

  int count() const { return count_; }
  int selected() const { return selected_; }
  int first() const { return first_; }  // index shown in the top row
  int visibleRows() const { return rows_; }
  bool canScrollUp() const { return first_ > 0; }
  bool canScrollDown() const { return first_ + rows_ < count_; }

 private:
  void follow();
  int count_ = 0;
  int rows_ = 1;
  int selected_ = 0;
  int first_ = 0;
};
