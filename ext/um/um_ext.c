#include "iou.h"

void Init_IOURing();
void Init_OpCtx();
void Init_UM();

void Init_um_ext(void) {
  Init_IOURing();
  Init_OpCtx();
  Init_UM();
}
