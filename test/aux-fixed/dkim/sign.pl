use Mail::DKIM::Signer;
use Mail::DKIM::TextWrap;  #recommended
use Getopt::Long;

my $method = "simple/simple";

GetOptions(
	"method=s" => \$method,
);

# create a signer object
my $dkim = Mail::DKIM::Signer->new(
                  Algorithm => "rsa-sha1",
                  Method => $method,
                  Domain => "test.ex",
                  Selector => "sel",
                  KeyFile => "aux-fixed/dkim/dkim.private",
             );

# read an email and pass it into the signer, one line at a time
while (<STDIN>)
{
      # remove local line terminators
      chomp;
      s/\015$//;

      # use SMTP line terminators
      $dkim->PRINT("$_\015\012");
}
$dkim->CLOSE;

# what is the signature result?
my $signature = $dkim->signature;
print $signature->as_string;
print "\n";

#print $dkim->headers;
#print "\n";
