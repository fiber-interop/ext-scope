<?php
// compiled only when included: its $_SERVER and $_REQUEST reads would arm a lazy build
return [isset($_SERVER['PATH']) ? 'env' : 'scope', $_SERVER['who'] ?? '-', $_REQUEST['r'] ?? '-'];
