# Sample configuration used in docs and tests.
# Demonstrates attributes, nested blocks, lists, and object literals.

variable "region" {
    default = "eu-west-1"
    type = string
}

resource "aws_instance" "web" {
    ami = "ami-12345"
    instance_type = "t2.micro"
    count = 3

    #Object literal: tag map.
    tags = {
        Name = "web"
        Env = "prod"
    }

    # List literal: multiple security groups.
    security_groups = ["default", "web"]
}