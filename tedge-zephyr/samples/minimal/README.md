# tedge-zephyr minimal sample

The smallest application that includes tedge-zephyr. It prints the module's
version. It uses nothing outside Zephyr and this module, so a successful build
shows the module is independent of any host repository.

```sh
west build -b native_sim/native/64 tedge-zephyr/samples/minimal
./build/zephyr/zephyr.exe
```
