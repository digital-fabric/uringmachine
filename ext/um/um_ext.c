void Init_UM();
void Init_Mutex();
void Init_Queue();

void Init_um_ext(void) {
  Init_UM();
  Init_Mutex();
  Init_Queue();
}
