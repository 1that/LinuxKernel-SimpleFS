# SimpleFS: инструкия
### Сборка
```
cd /kernal
make
```
```
cd /cli
make
```

### Диск
```
cd /kernal
dd if=/dev/zero of=disk.img bs=512 count=2048 status=none
LOOP=$(sudo losetup -f --show disk.img)
echo $LOOP
```

### Загрузка модуля
```
sudo insmod simple_file_system.ko device_name=$LOOP sb0_sector=0 sb1_sector=1024 max_file_sectors=8 name_max=14
```

### Монтирование
```
sudo mkdir -p /mnt/sfs
sudo mount -t simplefs $LOOP /mnt/sfs
```

### userspace
В каждый файл пишет случайное число и читает его обратно:
```
./userspace /mnt/sfs test
```
Вывод метаинформации:
```
./userspace  /mnt/sfs meta
```
Обнуление данных:
```
./userspace /mnt/sfs zero
```
Стереть ФС:
```
./userspace /mnt/sfs erase
```

### Ручная проверка
Просмотр файлов:
```
ls /mnt/sfs
```
Запись в файл:
```
echo text > /mnt/sfs/file0
```
Просмотр содержимого файла:
```
cat /mnt/sfs/file0
```

### Завершение работы
```
sudo umount /mnt/sfs
sudo rmmod simple_file_system
sudo losetup -d $LOOP
```
