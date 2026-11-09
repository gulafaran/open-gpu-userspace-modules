set breakpoint pending on
set confirm off
break nvmlInternalGetExportTable
commands
silent
set $out = (unsigned long*)$rdi
finish
end
run
set $t = *(unsigned long**)$out
printf "table=%p\n", $t
set $i = 0
while $i < 48
  printf "[%2d] %016lx  ", $i, $t[$i]
  info symbol $t[$i]
  set $i = $i + 1
end
kill
