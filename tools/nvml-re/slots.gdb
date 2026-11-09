set breakpoint pending on
set confirm off
set pagination off
break nvmlInternalGetExportTable
commands
silent
set $out = (unsigned long*)$rdi
finish
end
run
set $t = *(unsigned long**)$out
set $n = $t[0] / 8
set $i = 1
while $i < $n
  if $t[$i] > 0x10000
    eval "dprintf *0x%lx,\"SLOT %d\\n\"", $t[$i], $i
  end
  set $i = $i + 1
end
delete 1
continue
